/* Actual stock PAK bytes and bgfx Noop reference lifetimes. The public shader
 * loader has no Noop suffix: this fixture consumes the same private link owner
 * directly, without claiming physical GPU compilation, execution or pixels. */
#include "unity.h"
#include <jce/renderer/jce_renderer.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_filesystem.h>
#include <bgfx/c99/bgfx.h>
#include <SDL3/SDL_stdinc.h>
#include <jce/os/core/jce_timer.h>
#include "renderer/jce_renderer_lifecycle_internal.h"
#include "renderer/jce_renderer_bgfx_callback.h"
#include "os/core/jce_sha256_internal.h"
#include "os/core/jce_trace_capture_internal.h"
static bgfx_shader_handle_t observed_shader_create(const bgfx_memory_t *memory);
static void observed_sha256(const void *data, size_t size, uint8_t out[32]);
static uint64_t observed_shader_clock(void);
static bgfx_renderer_type_t observed_shader_renderer_type(void);
static void *observed_overlay_read(const char *path, uint64_t *size);
/* One production owner, not a copied linker. Its public definitions satisfy
 * the renderer archive's references, so the archive does not add a second TU.
 * Noop has no binary suffix: only this TU's type query can select a real stock
 * profile. Reads are counted and forwarded unchanged to the real host adapter. */
#define bgfx_get_renderer_type observed_shader_renderer_type
#define jce_fs_host_read_all observed_overlay_read
#ifndef JCE_TEST_SHADER_LINK_OWNER
#define JCE_TEST_SHADER_LINK_OWNER "renderer/jce_shaders.c"
#endif
#define bgfx_create_shader observed_shader_create
#define jce_sha256_internal observed_sha256
#define jce_time_perf_counter observed_shader_clock
#include JCE_TEST_SHADER_LINK_OWNER
#undef jce_time_perf_counter
#undef jce_sha256_internal
#undef bgfx_create_shader
#undef jce_fs_host_read_all
#undef bgfx_get_renderer_type

typedef struct { uint16_t shaders, programs, uniforms; } ResourceCounts;
static JceRenderer *renderer;
static JceAtomicI32 *fatal_count;
static bgfx_shader_handle_t stages[2];
static JceShaderHandle programs[2];
static void *source_bytes[2];
static const JcePakArchive *stock;
static const char *profile;
static ResourceCounts baseline;
static bgfx_renderer_type_t shader_type = BGFX_RENDERER_TYPE_NOOP;
static JceAtomicI32 *missing_overlay_errors, *shader_errors;
static const char overlay_directory[] = JCE_TEST_SHADER_OVERLAY_DIR;
static char overlay_file[512];
static unsigned overlay_reads;
static unsigned sha_calls, shader_clock_calls, create_api_calls;
static unsigned enable_trace_after_create;
static bool observe_payloads;
static struct { uint32_t size; uint8_t digest[32]; uint16_t handle; unsigned hashed_before; } observed_creates[8];
static unsigned observed_create_count;
#if !JCE_PLATFORM_WEB
static JceTraceEvent provenance_events[512];
static unsigned provenance_count;
#endif
static JceShaderSet stock_sets[3];
static bool stock_set_owned[3];

static uint64_t observed_shader_clock(void)
{
    ++shader_clock_calls;
    return jce_time_perf_counter();
}
static void observed_sha256(const void *data, size_t size, uint8_t out[32])
{
    ++sha_calls;
    jce_sha256_internal(data, size, out);
}
static bgfx_shader_handle_t observed_shader_create(const bgfx_memory_t *memory)
{
    ++create_api_calls;
    const unsigned index = observed_create_count;
    if (observe_payloads) {
        TEST_ASSERT_LESS_THAN_UINT32(8, index);
        observed_creates[index].size = memory->size;
        jce_sha256_internal(memory->data, memory->size, observed_creates[index].digest);
        observed_creates[index].hashed_before = sha_calls;
        ++observed_create_count;
    }
    const bgfx_shader_handle_t shader = bgfx_create_shader(memory);
    if (observe_payloads) observed_creates[index].handle = shader.idx;
    /* A capture can start after otherwise unobserved real stage creation.
     * The actual API result and device policy are forwarded unchanged. */
    if (enable_trace_after_create && create_api_calls == enable_trace_after_create)
        jce_trace_set_enabled(true);
    return shader;
}

#if !JCE_PLATFORM_WEB
/* Engine and fixture use the same CRT. Win32 SDL_setenv_unsafe updates the
 * OS environment table; that does not refresh the Engine's CRT getenv table. */
static void set_native_diag_environment(const char *value)
{
#if JCE_PLATFORM_WINDOWS
    TEST_ASSERT_EQUAL_INT(0, _putenv_s("JCE_RENDER_RESOURCE_DIAG", value));
#else
    TEST_ASSERT_EQUAL_INT(0, SDL_setenv_unsafe("JCE_RENDER_RESOURCE_DIAG", value, 1));
#endif
    TEST_ASSERT_EQUAL_STRING(value, getenv("JCE_RENDER_RESOURCE_DIAG"));
}
#endif

static bgfx_renderer_type_t observed_shader_renderer_type(void)
{
    return shader_type == BGFX_RENDERER_TYPE_NOOP ? bgfx_get_renderer_type() : shader_type;
}

static void *observed_overlay_read(const char *path, uint64_t *size)
{
    if (overlay_file[0] && strcmp(path, overlay_file) == 0) ++overlay_reads;
    return jce_fs_host_read_all(path, size);
}

static void observe_fatal(const JceLogRecord *record, void *user)
{
    if (record->level == JCE_LOG_LEVEL_ERROR &&
        strcmp(record->tag, "jce_renderer") == 0 &&
        strncmp(record->message, "bgfx fatal:", 11) == 0)
        jce_atomic_i32_add((JceAtomicI32 *)user, 1);
    if (record->level == JCE_LOG_LEVEL_ERROR && strcmp(record->tag, "jce_shaders") == 0) {
        jce_atomic_i32_add(shader_errors, 1);
        if (strncmp(record->message, "fs open/read failed:", 20) == 0 &&
            strstr(record->message, overlay_directory))
            jce_atomic_i32_add(missing_overlay_errors, 1);
    }
}

static ResourceCounts counts(void)
{
    /* Headless hosts intentionally do not pump end_frame. This is actual Noop
     * command delivery, not an invented production loop or GPU completion. */
    for (unsigned i = 0; i < 6; ++i) bgfx_frame(false);
    const bgfx_stats_t *stats = bgfx_get_stats();
    TEST_ASSERT_NOT_NULL(stats);
    return (ResourceCounts){stats->numShaders, stats->numPrograms, stats->numUniforms};
}

static void baseline_restored(void)
{
    ResourceCounts actual = counts();
    TEST_ASSERT_EQUAL_UINT16(baseline.shaders, actual.shaders);
    TEST_ASSERT_EQUAL_UINT16(baseline.programs, actual.programs);
    TEST_ASSERT_EQUAL_UINT16(baseline.uniforms, actual.uniforms);
}

static bool stock_has_pair(const char *suffix)
{
    static const char *names[] = {"vs_grass", "fs_grass_shadow", "fs_shadow"};
    char key[96];
    for (unsigned i = 0; i < 3; ++i) {
        snprintf(key, sizeof key, "shaders/%s_%s.bin", names[i], suffix);
        if (!jce_pak_find(stock, key)) return false;
    }
    return true;
}

static void expect_trace_quiescent(void)
{
    JceTraceStats stats;
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_work_items);
    TEST_ASSERT_EQUAL_UINT64(0, jce_trace_task_current_id());
}

static void expect_synchronous_noop_callbacks(void)
{
    const JceBgfxCpuProfilerWitness before = jce_rcb_cpu_profiler_witness();
    if (before.compiled != 1 || !before.bridge_supported) {
        printf("Noop CPU callback witness unavailable: compiled=%d bridge=%d\n",
            (int)before.compiled, (int)before.bridge_supported);
        return;
    }
    const bool enabled = jce_trace_enabled();
    const uint64_t caller = jce_thread_current_id();
    JceTraceCursor cursor;
    JceTraceEvent events[32];
    jce_trace_cursor_init(&cursor);
    while (jce_trace_read(&cursor, events, 32)) { }
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
    jce_trace_set_enabled(true);
    (void)counts(); /* Actual synchronous Noop frame/callback delivery. */
    struct { uint64_t id; bool api, render; } open[64];
    unsigned used = 0, api_pairs = 0, render_pairs = 0;
    uint32_t count;
    while ((count = jce_trace_read(&cursor, events, 32)) != 0) {
        for (uint32_t i = 0; i < count; ++i) {
            const JceTraceEvent *event = &events[i];
            if (strncmp(event->name, "bgfx.cpu.", 9) != 0) continue;
            TEST_ASSERT_FALSE(strcmp(event->name, "bgfx.cpu.stack_overflow") == 0);
            if (event->type != JCE_TRACE_EVENT_TASK_BEGIN &&
                event->type != JCE_TRACE_EVENT_TASK_END) continue;
            TEST_ASSERT_EQUAL_UINT64(caller, event->thread_id);
            TEST_ASSERT_NOT_EQUAL_UINT64(0, event->id);
            const bool api = strcmp(event->name, "bgfx.cpu.bgfx/API thread frame") == 0;
            const bool render = strcmp(event->name, "bgfx.cpu.bgfx::renderFrame") == 0;
            if (event->type == JCE_TRACE_EVENT_TASK_BEGIN) {
                TEST_ASSERT_LESS_THAN_UINT32(64, used);
                open[used].id = event->id;
                open[used].api = api; open[used].render = render;
                ++used;
            } else {
                TEST_ASSERT_GREATER_THAN_UINT32(0, used);
                --used;
                TEST_ASSERT_EQUAL_UINT64(open[used].id, event->id);
                TEST_ASSERT_EQUAL_INT(open[used].api, api);
                TEST_ASSERT_EQUAL_INT(open[used].render, render);
                if (api) ++api_pairs;
                if (render) ++render_pairs;
            }
        }
    }
    TEST_ASSERT_EQUAL_UINT32(0, used);
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
    TEST_ASSERT_GREATER_THAN_UINT32(0, api_pairs);
    TEST_ASSERT_GREATER_THAN_UINT32(0, render_pairs);
    const JceBgfxCpuProfilerWitness after = jce_rcb_cpu_profiler_witness();
    TEST_ASSERT_EQUAL_INT(1, after.compiled);
    TEST_ASSERT_TRUE(after.bridge_supported);
    TEST_ASSERT_GREATER_THAN_UINT32(0, after.begin_observed);
    expect_trace_quiescent();
    jce_trace_set_enabled(enabled);
}

static void create_synchronous_noop(void)
{
    TEST_ASSERT_NULL(renderer);
    expect_trace_quiescent();
    /* Pinned bgfx: pre-init render_frame and init on this API thread select
     * synchronous frame delivery. No live backend producer crosses resets. */
    TEST_ASSERT_EQUAL_INT(BGFX_RENDER_FRAME_NO_CONTEXT, bgfx_render_frame(-1));
    renderer = jce_renderer_create_headless();
    TEST_ASSERT_NOT_NULL(renderer);
    expect_trace_quiescent();
    expect_synchronous_noop_callbacks();
}

void setUp(void)
{
    stages[0].idx = stages[1].idx = UINT16_MAX;
    programs[0] = programs[1] = JCE_INVALID_SHADER;
    profile = NULL;
    shader_type = BGFX_RENDERER_TYPE_NOOP;
    overlay_file[0] = '\0';
    overlay_reads = 0;
    memset(stock_set_owned, 0, sizeof stock_set_owned);
    fatal_count = jce_atomic_i32_create(0);
    missing_overlay_errors = jce_atomic_i32_create(0);
    shader_errors = jce_atomic_i32_create(0);
    TEST_ASSERT_NOT_NULL(fatal_count);
    TEST_ASSERT_NOT_NULL(missing_overlay_errors);
    TEST_ASSERT_NOT_NULL(shader_errors);
    jce_log_set_sink(observe_fatal, fatal_count);
    /* Real native environment and actual Noop device session latch: no DIAG
     * provider substitutes for the renderer owner in this fixture. */
#if !JCE_PLATFORM_WEB
    set_native_diag_environment("1");
#endif
    observe_payloads = false;
    enable_trace_after_create = 0;
    create_synchronous_noop();
    TEST_ASSERT_NOT_NULL(renderer);
    TEST_ASSERT_EQUAL_INT(BGFX_RENDERER_TYPE_NOOP, bgfx_get_renderer_type());
    stock = jce_shaders_embedded_engine_pak();
    TEST_ASSERT_NOT_NULL_MESSAGE(stock, "actual embedded stock shader PAK unavailable");
    static const char *profiles[] = {"dx11", "glsl", "spv", "essl", "mtl"};
    for (unsigned i = 0; i < sizeof profiles / sizeof profiles[0]; ++i)
        if (stock_has_pair(profiles[i])) { profile = profiles[i]; break; }
    TEST_ASSERT_NOT_NULL_MESSAGE(profile, "actual complete grass/shadow shader profile unavailable");
    TEST_ASSERT_GREATER_THAN_INT(0, snprintf(overlay_file, sizeof overlay_file,
        "%s/shaders/fs_color_%s.bin", overlay_directory, profile));
    TEST_ASSERT_TRUE(jce_fs_host_remove_file(overlay_file));
    baseline = counts();
    expect_trace_quiescent();
    jce_trace_set_enabled(false);
    jce_trace_reset();
    jce_trace_set_enabled(true);
    sha_calls = shader_clock_calls = create_api_calls = observed_create_count = 0;
}

void tearDown(void)
{
    jce_trace_set_enabled(false);
    for (unsigned i = 0; i < 3; ++i)
        if (stock_set_owned[i]) {
            jce_shader_set_release_owned(&stock_sets[i]);
            stock_set_owned[i] = false;
        }
    if (overlay_file[0]) jce_fs_host_remove_file(overlay_file);
    for (unsigned i = 0; i < 2; ++i) {
        JCE_FREE(source_bytes[i]); source_bytes[i] = NULL;
        if (stages[i].idx != UINT16_MAX) bgfx_destroy_shader(stages[i]);
        stages[i].idx = UINT16_MAX;
        jce_shader_program_destroy(programs[i]); programs[i] = JCE_INVALID_SHADER;
    }
    if (renderer) { jce_renderer_destroy(renderer); renderer = NULL; }
    expect_trace_quiescent();
    int32_t actual = fatal_count ? jce_atomic_i32_load(fatal_count) : 0;
    jce_log_set_sink(NULL, NULL);
    if (fatal_count) { jce_atomic_i32_destroy(fatal_count); fatal_count = NULL; }
    if (missing_overlay_errors) {
        jce_atomic_i32_destroy(missing_overlay_errors); missing_overlay_errors = NULL;
    }
    if (shader_errors) { jce_atomic_i32_destroy(shader_errors); shader_errors = NULL; }
    TEST_ASSERT_EQUAL_INT32_MESSAGE(0, actual, "actual Noop fatal observed");
    stock = NULL;
}

static uint32_t read_stage_blob(unsigned slot, const char *name)
{
    char key[96];
    snprintf(key, sizeof key, "shaders/%s_%s.bin", name, profile);
    const JcePakAsset *asset = jce_pak_find(stock, key);
    TEST_ASSERT_NOT_NULL(asset);
    TEST_ASSERT_TRUE(asset->original_size > 0 && asset->original_size <= UINT32_MAX);
    source_bytes[slot] = JCE_MALLOC((size_t)asset->original_size);
    TEST_ASSERT_NOT_NULL(source_bytes[slot]);
    TEST_ASSERT_EQUAL_UINT64(asset->original_size,
        jce_pak_decompress(asset, source_bytes[slot], (size_t)asset->original_size));
    TEST_ASSERT_EQUAL_INT(1, jce_pak_verify(asset, source_bytes[slot], (size_t)asset->original_size));
    return (uint32_t)asset->original_size;
}

static void read_stage(unsigned slot, const char *name)
{
    const uint32_t size = read_stage_blob(slot, name);
    const bgfx_memory_t *memory = bgfx_copy(source_bytes[slot], size);
    JCE_FREE(source_bytes[slot]); source_bytes[slot] = NULL;
    TEST_ASSERT_NOT_NULL(memory);
    stages[slot] = bgfx_create_shader(memory);
    TEST_ASSERT_NOT_EQUAL_UINT16(UINT16_MAX, stages[slot].idx);
}

static void link_pair(unsigned slot, const char *fragment)
{
    read_stage(0, "vs_grass"); read_stage(1, fragment);
    bgfx_program_handle_t program = link_program_owned(stages[0], stages[1]);
    /* The exact link owner consumes both creator refs, even on mismatch. */
    stages[0].idx = stages[1].idx = UINT16_MAX;
    programs[slot] = (JceShaderHandle){program.idx};
}

static void expect_link_trace(unsigned expected_scopes, JceTraceTaskState outcome,
                              uint64_t rejection)
{
    static const char *names[] = {"shader.link.vs", "shader.link.fs", "shader.link.program",
        "shader.link.creator_releases", "shader.link.success", "shader.link.rejection"};
    JceTraceCursor cursor;
    JceTraceEvent events[32];
    unsigned columns = 0, completed = 0;
    uint64_t identity = 0, parent = 0;
    jce_trace_cursor_init(&cursor);
    uint32_t count;
    while ((count = jce_trace_read(&cursor, events, 32)) != 0u) {
        for (uint32_t i = 0; i < count; ++i) {
            const JceTraceEvent *event = &events[i];
            if (event->type == JCE_TRACE_EVENT_COUNTER &&
                strncmp(event->name, "shader.link.", 12) == 0) {
                TEST_ASSERT_LESS_THAN_UINT32(6, columns);
                TEST_ASSERT_EQUAL_STRING(names[columns], event->name);
                if (columns == 0) { identity = event->id; parent = event->parent_id; }
                TEST_ASSERT_NOT_EQUAL_UINT64(0, identity);
                TEST_ASSERT_EQUAL_UINT64(identity, event->id);
                TEST_ASSERT_EQUAL_UINT64(parent, event->parent_id);
                if (columns < 2) TEST_ASSERT_NOT_EQUAL_UINT64(UINT16_MAX, event->value_u64);
                if (columns == 2) TEST_ASSERT_EQUAL_UINT64(outcome == JCE_TRACE_TASK_SUCCEEDED,
                    event->value_u64 != UINT16_MAX);
                if (columns == 3) TEST_ASSERT_EQUAL_UINT64(2, event->value_u64);
                if (columns == 4) TEST_ASSERT_EQUAL_UINT64(outcome == JCE_TRACE_TASK_SUCCEEDED, event->value_u64);
                if (columns == 5) TEST_ASSERT_EQUAL_UINT64(rejection, event->value_u64);
                ++columns;
            } else if (event->type == JCE_TRACE_EVENT_TASK_END &&
                       strcmp(event->name, "renderer.shader.link") == 0) {
                TEST_ASSERT_EQUAL_UINT32(6, columns);
                TEST_ASSERT_EQUAL_UINT64(identity, event->id);
                TEST_ASSERT_EQUAL_UINT32(outcome, event->value_u32[0]);
                columns = 0; ++completed;
            }
        }
    }
    TEST_ASSERT_EQUAL_UINT32(0, columns);
    TEST_ASSERT_EQUAL_UINT32(expected_scopes, completed);
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
    JceTraceStats stats;
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_work_items);
    TEST_ASSERT_EQUAL_UINT64(0, jce_trace_task_current_id());
}

static void test_actual_matching_stock_pair_retains_program_until_public_destroy(void)
{
    link_pair(0, "fs_grass_shadow");
    TEST_ASSERT_TRUE(jce_shader_valid(programs[0]));
    ResourceCounts linked = counts();
    TEST_ASSERT_EQUAL_UINT16(baseline.programs + 1, linked.programs);
    TEST_ASSERT_EQUAL_UINT16(baseline.shaders + 2, linked.shaders);
    expect_link_trace(1, JCE_TRACE_TASK_SUCCEEDED, 0);
    jce_shader_program_destroy(programs[0]); programs[0] = JCE_INVALID_SHADER;
    baseline_restored();
}

static void test_actual_mismatched_stock_pair_rejects_and_restores_all_baselines(void)
{
    link_pair(0, "fs_shadow");
    TEST_ASSERT_FALSE(jce_shader_valid(programs[0]));
    expect_link_trace(1, JCE_TRACE_TASK_FAILED, 2);
    baseline_restored();
}

static JceShaderHandle link_public_blob_pair(const char *fragment)
{
    const uint32_t vs_size = read_stage_blob(0, "vs_grass");
    const uint32_t fs_size = read_stage_blob(1, fragment);
    const JceShaderHandle program = jce_renderer_create_program_from_blobs(
        source_bytes[0], vs_size, source_bytes[1], fs_size);
    for (unsigned i = 0; i < 2; ++i) {
        JCE_FREE(source_bytes[i]);
        source_bytes[i] = NULL;
    }
    return program;
}

static void test_public_blob_pair_links_and_releases(void)
{
    programs[0] = link_public_blob_pair("fs_grass_shadow");
    TEST_ASSERT_TRUE(jce_shader_valid(programs[0]));
    TEST_ASSERT_EQUAL_UINT16(baseline.programs + 1, counts().programs);
    expect_link_trace(1, JCE_TRACE_TASK_SUCCEEDED, 0);
    jce_renderer_destroy_program(programs[0]);
    programs[0] = JCE_INVALID_SHADER;
    baseline_restored();
}

static void test_public_blob_pair_releases_mismatched_stages(void)
{
    programs[0] = link_public_blob_pair("fs_shadow");
    TEST_ASSERT_FALSE(jce_shader_valid(programs[0]));
    expect_link_trace(1, JCE_TRACE_TASK_FAILED, 2);
    baseline_restored();
}

static void test_actual_cached_program_has_two_independent_acquires(void)
{
    link_pair(0, "fs_grass_shadow"); link_pair(1, "fs_grass_shadow");
    TEST_ASSERT_TRUE(jce_shader_valid(programs[0]));
    TEST_ASSERT_EQUAL_UINT16(programs[0].idx, programs[1].idx);
    ResourceCounts linked = counts();
    TEST_ASSERT_EQUAL_UINT16(baseline.programs + 1, linked.programs);
    TEST_ASSERT_EQUAL_UINT16(baseline.shaders + 2, linked.shaders);
    expect_link_trace(2, JCE_TRACE_TASK_SUCCEEDED, 0);
    jce_shader_program_destroy(programs[0]); programs[0] = JCE_INVALID_SHADER;
    ResourceCounts remaining = counts();
    TEST_ASSERT_EQUAL_UINT16(linked.programs, remaining.programs);
    TEST_ASSERT_EQUAL_UINT16(linked.shaders, remaining.shaders);
    TEST_ASSERT_EQUAL_UINT16(linked.uniforms, remaining.uniforms);
    jce_shader_program_destroy(programs[1]); programs[1] = JCE_INVALID_SHADER;
    baseline_restored();
}

static void test_actual_trace_off_keeps_ownership_and_emits_no_link_records(void)
{
    jce_trace_set_enabled(false);
    link_pair(0, "fs_grass_shadow");
    TEST_ASSERT_TRUE(jce_shader_valid(programs[0]));
    jce_shader_program_destroy(programs[0]); programs[0] = JCE_INVALID_SHADER;
    baseline_restored();
    expect_link_trace(0, JCE_TRACE_TASK_SUCCEEDED, 0);
}

static void enable_stock_loaders(void)
{
    jce_trace_set_enabled(false);
    if (strcmp(profile, "dx11") == 0) shader_type = BGFX_RENDERER_TYPE_DIRECT3D11;
    else if (strcmp(profile, "glsl") == 0) shader_type = BGFX_RENDERER_TYPE_OPENGL;
    else if (strcmp(profile, "spv") == 0) shader_type = BGFX_RENDERER_TYPE_VULKAN;
    else if (strcmp(profile, "essl") == 0) shader_type = BGFX_RENDERER_TYPE_OPENGLES;
    else shader_type = BGFX_RENDERER_TYPE_METAL;
    TEST_ASSERT_EQUAL_INT(BGFX_RENDERER_TYPE_NOOP, bgfx_get_renderer_type());
}

static void write_fragment_overlay_variant(bool reject_magic)
{
    char key[96];
    snprintf(key, sizeof key, "shaders/fs_color_%s.bin", profile);
    const JcePakAsset *asset = jce_pak_find(stock, key);
    TEST_ASSERT_NOT_NULL(asset);
    source_bytes[0] = JCE_MALLOC((size_t)asset->original_size);
    TEST_ASSERT_NOT_NULL(source_bytes[0]);
    TEST_ASSERT_EQUAL_UINT64(asset->original_size,
        jce_pak_decompress(asset, source_bytes[0], (size_t)asset->original_size));
    TEST_ASSERT_EQUAL_INT(1, jce_pak_verify(asset, source_bytes[0], (size_t)asset->original_size));
    if (reject_magic) {
        /* Real stock carrier/size/contents, with only its four-byte magic
         * rejected by the pinned bgfx Context before shader allocation. */
        static const uint8_t invalid_magic[4] = {'B','A','D','!'};
        TEST_ASSERT_TRUE(asset->original_size >= sizeof invalid_magic);
        memcpy(source_bytes[0], invalid_magic, sizeof invalid_magic);
    }
    TEST_ASSERT_TRUE(jce_fs_host_write_all(overlay_file, source_bytes[0], asset->original_size));
    JCE_FREE(source_bytes[0]); source_bytes[0] = NULL;
}

static void write_actual_fragment_overlay(void)
{
    write_fragment_overlay_variant(false);
}

/* Exercise the compiled renderer startup wire. Only the existing shader TU's
 * suffix port selects stock bytes; the renderer remains the real Noop owner. */
static void expect_observed_stock_payload(unsigned index, const char *stage)
{
    char key[96];
    snprintf(key, sizeof key, "shaders/%s_%s.bin", stage, profile);
    const JcePakAsset *asset = jce_pak_find(stock, key);
    TEST_ASSERT_NOT_NULL(asset);
    source_bytes[0] = JCE_MALLOC((size_t)asset->original_size);
    TEST_ASSERT_NOT_NULL(source_bytes[0]);
    TEST_ASSERT_EQUAL_UINT64(asset->original_size,
        jce_pak_decompress(asset, source_bytes[0], (size_t)asset->original_size));
    TEST_ASSERT_EQUAL_INT(1, jce_pak_verify(asset, source_bytes[0], (size_t)asset->original_size));
    uint8_t digest[32];
    jce_sha256_internal(source_bytes[0], (size_t)asset->original_size, digest);
    TEST_ASSERT_TRUE(index < observed_create_count);
    TEST_ASSERT_EQUAL_UINT32(asset->original_size, observed_creates[index].size);
    TEST_ASSERT_EQUAL_MEMORY(digest, observed_creates[index].digest, sizeof digest);
    JCE_FREE(source_bytes[0]); source_bytes[0] = NULL;
}
static void expect_overlay_stage_absent(const char *stage)
{
    char path[512];
    TEST_ASSERT_GREATER_THAN_INT(0, snprintf(path, sizeof path,
        "%s/shaders/%s_%s.bin", overlay_directory, stage, profile));
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(path));
}
static void expect_startup_pak_ignores_existing_overlay(void)
{
    enable_stock_loaders();
    write_actual_fragment_overlay();
    observe_payloads = true;
    programs[0] = jce_renderer_shader_load_named_internal(stock, "color", "color");
    TEST_ASSERT_TRUE(jce_shader_valid(programs[0]));
    TEST_ASSERT_EQUAL_UINT32(0, overlay_reads);
    TEST_ASSERT_EQUAL_UINT32(2, observed_create_count);
    expect_observed_stock_payload(0, "vs_color");
    expect_observed_stock_payload(1, "fs_color");
    jce_shader_program_destroy(programs[0]); programs[0] = JCE_INVALID_SHADER;
    baseline_restored();
}
static void test_startup_unbound_shader_load_uses_pak_without_overlay(void)
{
    expect_startup_pak_ignores_existing_overlay();
}
static void test_startup_null_root_uses_pak_without_overlay(void)
{
    TEST_ASSERT_TRUE(jce_renderer_shader_dev_bind_internal(renderer, NULL));
    expect_startup_pak_ignores_existing_overlay();
}
static void test_startup_empty_root_uses_pak_without_overlay(void)
{
    TEST_ASSERT_TRUE(jce_renderer_shader_dev_bind_internal(renderer, ""));
    expect_startup_pak_ignores_existing_overlay();
}
static void test_startup_owned_root_reads_fragment_and_preserves_named_stage_pair(void)
{
    enable_stock_loaders();
    write_actual_fragment_overlay();
    expect_overlay_stage_absent("vs_color");
    expect_overlay_stage_absent("vs_grass");
    expect_overlay_stage_absent("fs_grass_shadow");
    char caller_root[sizeof overlay_directory];
    memcpy(caller_root, overlay_directory, sizeof caller_root);
    TEST_ASSERT_TRUE(jce_renderer_shader_dev_bind_internal(renderer, caller_root));
    memset(caller_root, 'x', sizeof caller_root - 1);
    observe_payloads = true;
    programs[0] = jce_renderer_shader_load_named_internal(NULL, "color", "color");
    TEST_ASSERT_TRUE(jce_shader_valid(programs[0]));
    TEST_ASSERT_EQUAL_UINT32(1, overlay_reads);
    TEST_ASSERT_EQUAL_UINT32(2, observed_create_count);
    expect_observed_stock_payload(0, "vs_color");
    expect_observed_stock_payload(1, "fs_color");
    programs[1] = jce_renderer_shader_load_named_internal(stock, "grass", "grass_shadow");
    TEST_ASSERT_TRUE(jce_shader_valid(programs[1]));
    TEST_ASSERT_EQUAL_UINT32(1, overlay_reads);
    TEST_ASSERT_EQUAL_UINT32(4, observed_create_count);
    expect_observed_stock_payload(2, "vs_grass");
    expect_observed_stock_payload(3, "fs_grass_shadow");
    jce_log_flush();
    TEST_ASSERT_EQUAL_INT32(0, jce_atomic_i32_load(missing_overlay_errors));
    for (unsigned i = 0; i < 2; ++i) {
        jce_shader_program_destroy(programs[i]); programs[i] = JCE_INVALID_SHADER;
    }
    baseline_restored();
}
static void test_startup_existing_empty_overlay_fails_closed_and_releases_creator(void)
{
    enable_stock_loaders();
    TEST_ASSERT_TRUE(jce_fs_host_write_all(overlay_file, "", 0));
    TEST_ASSERT_TRUE(jce_fs_host_exists_file(overlay_file));
    TEST_ASSERT_TRUE(jce_renderer_shader_dev_bind_internal(renderer, overlay_directory));
    observe_payloads = true;
    programs[0] = jce_renderer_shader_load_named_internal(NULL, "color", "color");
    TEST_ASSERT_FALSE(jce_shader_valid(programs[0]));
    TEST_ASSERT_EQUAL_UINT32(1, overlay_reads);
    TEST_ASSERT_EQUAL_UINT32(1, observed_create_count);
    expect_observed_stock_payload(0, "vs_color");
    TEST_ASSERT_NOT_EQUAL_UINT16(UINT16_MAX, observed_creates[0].handle);
    jce_log_flush();
    TEST_ASSERT_GREATER_THAN_INT32(0, jce_atomic_i32_load(shader_errors));
    baseline_restored();
}
static void test_startup_existing_bad_overlay_fails_closed_and_releases_creators(void)
{
    enable_stock_loaders();
    write_fragment_overlay_variant(true);
    TEST_ASSERT_TRUE(jce_renderer_shader_dev_bind_internal(renderer, overlay_directory));
    observe_payloads = true;
    programs[0] = jce_renderer_shader_load_named_internal(NULL, "color", "color");
    TEST_ASSERT_FALSE(jce_shader_valid(programs[0]));
    TEST_ASSERT_EQUAL_UINT32(1, overlay_reads);
    TEST_ASSERT_EQUAL_UINT32(2, observed_create_count);
    expect_observed_stock_payload(0, "vs_color");
    TEST_ASSERT_NOT_EQUAL_UINT16(UINT16_MAX, observed_creates[0].handle);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, observed_creates[1].handle);
    jce_log_flush();
    TEST_ASSERT_GREATER_THAN_INT32(0, jce_atomic_i32_load(shader_errors));
    baseline_restored();
}
static void test_new_renderer_after_destroy_does_not_inherit_startup_root(void)
{
    enable_stock_loaders();
    write_actual_fragment_overlay();
    TEST_ASSERT_TRUE(jce_renderer_shader_dev_bind_internal(renderer, overlay_directory));
    programs[0] = jce_renderer_shader_load_named_internal(NULL, "color", "color");
    TEST_ASSERT_TRUE(jce_shader_valid(programs[0]));
    TEST_ASSERT_EQUAL_UINT32(1, overlay_reads);
    jce_shader_program_destroy(programs[0]); programs[0] = JCE_INVALID_SHADER;
    baseline_restored();
    jce_renderer_destroy(renderer); renderer = NULL;
    create_synchronous_noop();
    TEST_ASSERT_NOT_NULL(renderer);
    TEST_ASSERT_EQUAL_INT(BGFX_RENDERER_TYPE_NOOP, bgfx_get_renderer_type());
    enable_stock_loaders();
    baseline = counts();
    programs[0] = jce_renderer_shader_load_named_internal(NULL, "color", "color");
    TEST_ASSERT_TRUE(jce_shader_valid(programs[0]));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, overlay_reads,
        "a new renderer must use the default PAK route until its own root is bound");
    jce_shader_program_destroy(programs[0]); programs[0] = JCE_INVALID_SHADER;
    baseline_restored();
}

static void release_stock_set(unsigned slot)
{
    jce_shader_set_release_owned(&stock_sets[slot]);
    stock_set_owned[slot] = false;
}

static void test_fragment_only_stock_overlay_uses_embedded_vertex_without_missing_errors(void)
{
    enable_stock_loaders();
    write_actual_fragment_overlay();
    programs[0] = shader_load_program_named(NULL, "color", "color");
    TEST_ASSERT_TRUE(jce_shader_valid(programs[0]));
    stock_sets[0] = jce_shaders_load_all_fs(overlay_directory, NULL);
    stock_set_owned[0] = true;
    jce_log_flush();
    TEST_ASSERT_EQUAL_INT32_MESSAGE(0, jce_atomic_i32_load(missing_overlay_errors),
        "absent overlay stages must fall back quietly instead of blind file reads");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, overlay_reads, "actual fragment overlay was not consumed");
    TEST_ASSERT_TRUE(jce_shader_valid(stock_sets[0].color));
    TEST_ASSERT_EQUAL_UINT16(programs[0].idx, stock_sets[0].color.idx);
    release_stock_set(0);
    TEST_ASSERT_TRUE(jce_shader_valid(programs[0]));
    jce_shader_program_destroy(programs[0]); programs[0] = JCE_INVALID_SHADER;
    baseline_restored();
}

static void test_existing_empty_stock_overlay_is_not_replaced_by_baseline(void)
{
    enable_stock_loaders();
    TEST_ASSERT_TRUE(jce_fs_host_write_all(overlay_file, "", 0));
    TEST_ASSERT_TRUE(jce_fs_host_exists_file(overlay_file));
    stock_sets[0] = jce_shaders_load_all_fs(overlay_directory, NULL);
    stock_set_owned[0] = true;
    jce_log_flush();
    TEST_ASSERT_FALSE_MESSAGE(jce_shader_valid(stock_sets[0].color),
        "existing invalid fragment overlay must fail closed, not silently load the baseline program");
    TEST_ASSERT_EQUAL_UINT32(1, overlay_reads);
    TEST_ASSERT_GREATER_THAN_INT32(0, jce_atomic_i32_load(shader_errors));
    release_stock_set(0);
    baseline_restored();
}

static void test_null_and_empty_stock_overlay_preserve_cached_acquires_and_aliases(void)
{
    enable_stock_loaders();
    stock_sets[0] = jce_shaders_load_all(NULL); stock_set_owned[0] = true;
    ResourceCounts one = counts();
    stock_sets[1] = jce_shaders_load_all_fs(NULL, NULL); stock_set_owned[1] = true;
    stock_sets[2] = jce_shaders_load_all_fs("", NULL); stock_set_owned[2] = true;
    for (unsigned slot = 1; slot < 3; ++slot) {
#define SAME_STOCK_PROGRAM(member, renderer_member) \
        TEST_ASSERT_EQUAL_UINT16(stock_sets[0].member.idx, stock_sets[slot].member.idx);
        JCE_SHADER_SET_OWNED_FIELDS(SAME_STOCK_PROGRAM)
#undef SAME_STOCK_PROGRAM
        for (int v = 0; v < JCE_SHADER_VARIANT_COUNT; ++v)
            for (int k = 0; k < JCE_SHADER_KEY_COUNT; ++k)
                TEST_ASSERT_EQUAL_UINT16(stock_sets[0].variant[v][k].idx, stock_sets[slot].variant[v][k].idx);
    }
    release_stock_set(0);
    ResourceCounts two = counts();
    TEST_ASSERT_EQUAL_UINT16(one.shaders, two.shaders);
    TEST_ASSERT_EQUAL_UINT16(one.programs, two.programs);
    TEST_ASSERT_EQUAL_UINT16(one.uniforms, two.uniforms);
    release_stock_set(1);
    ResourceCounts last = counts();
    TEST_ASSERT_EQUAL_UINT16(one.shaders, last.shaders);
    TEST_ASSERT_EQUAL_UINT16(one.programs, last.programs);
    TEST_ASSERT_EQUAL_UINT16(one.uniforms, last.uniforms);
    release_stock_set(2);
    baseline_restored();
    jce_log_flush();
    TEST_ASSERT_EQUAL_INT32(0, jce_atomic_i32_load(missing_overlay_errors));
    TEST_ASSERT_EQUAL_UINT32(0, overlay_reads);
}

#if !JCE_PLATFORM_WEB
/* Payload expectations come from the actual Memory supplied to the real Noop
 * API. The shared SHA algorithm is independently anchored by Core KAT vectors.
 * These checks establish CPU calls, not a native GPU driver/device policy. */
static void start_provenance(void)
{
    enable_stock_loaders();
    TEST_ASSERT_TRUE(jce_renderer_shader_provenance_policy_internal());
    jce_trace_reset(); jce_trace_set_enabled(true);
    sha_calls = shader_clock_calls = create_api_calls = observed_create_count = 0;
    observe_payloads = true;
}
static void read_provenance(void)
{
    JceTraceCursor cursor; jce_trace_cursor_init(&cursor);
    provenance_count = 0;
    uint32_t count;
    do {
        TEST_ASSERT_LESS_THAN_UINT32(512, provenance_count);
        count = jce_trace_read(&cursor, provenance_events+provenance_count, 512-provenance_count);
        provenance_count += count;
    } while (count);
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
    JceTraceStats stats; jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_work_items);
    TEST_ASSERT_EQUAL_UINT64(0, jce_trace_task_current_id());
}
static void expect_no_new_provenance(void)
{
    /* Existing Pak::Decompress/Core/link events are independent observability;
     * keep tracing on and constrain only this default-off producer's packets. */
    for (unsigned i = 0; i < provenance_count; ++i) {
        const JceTraceEvent *event = &provenance_events[i];
        const bool new_counter = event->type == JCE_TRACE_EVENT_COUNTER &&
            (strncmp(event->name, "shader.create.", 14) == 0 ||
             strncmp(event->name, "shader.owner.", 13) == 0 ||
             strncmp(event->name, "shader.release.", 15) == 0);
        const bool new_scope = strcmp(event->name, "renderer.shader.create") == 0 ||
            strcmp(event->name, "renderer.shader.load_link") == 0 ||
            strcmp(event->name, "renderer.shader.creator_release") == 0;
        TEST_ASSERT_FALSE_MESSAGE(new_counter || new_scope, event->name);
    }
}
static uint64_t provenance_scope(const char *name, unsigned ordinal)
{
    for (unsigned i = 0; i < provenance_count; ++i)
        if (provenance_events[i].type == JCE_TRACE_EVENT_TASK_END &&
            strcmp(provenance_events[i].name, name) == 0 && ordinal-- == 0)
            return provenance_events[i].id;
    TEST_FAIL_MESSAGE("Expected complete provenance scope missing");
    return 0;
}
static uint64_t provenance_value(uint64_t scope, const char *name)
{
    unsigned matches = 0; uint64_t value = 0;
    for (unsigned i = 0; i < provenance_count; ++i)
        if (provenance_events[i].type == JCE_TRACE_EVENT_COUNTER &&
            provenance_events[i].id == scope && strcmp(provenance_events[i].name, name) == 0) {
            ++matches; value = provenance_events[i].value_u64;
        }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, matches, name);
    return value;
}
static uint64_t provenance_create(unsigned index)
{
    const uint64_t scope = provenance_scope("renderer.shader.create", index);
    TEST_ASSERT_EQUAL_UINT64(jce_trace_capture_generation(), provenance_value(scope, "shader.create.capture"));
    TEST_ASSERT_EQUAL_UINT64(1, provenance_value(scope, "shader.create.context_known"));
    uint64_t context = 0; uint32_t frame = 0;
    TEST_ASSERT_TRUE(jce_renderer_gpu_observation_context_internal(&context, &frame));
    TEST_ASSERT_EQUAL_UINT64(context >> 32, provenance_value(scope, "shader.create.context_hi"));
    TEST_ASSERT_EQUAL_UINT64((uint32_t)context, provenance_value(scope, "shader.create.context_lo"));
    TEST_ASSERT_EQUAL_UINT64(observed_creates[index].size, provenance_value(scope, "shader.create.bytes"));
    TEST_ASSERT_EQUAL_UINT64(1, provenance_value(scope, "shader.create.payload_known"));
    for (unsigned word = 0; word < 8; ++word) {
        char name[40]; snprintf(name, sizeof name, "shader.create.sha256.%u", word);
        const uint8_t *b = observed_creates[index].digest+4*word;
        const uint32_t expected = (uint32_t)b[0]<<24 | (uint32_t)b[1]<<16 | (uint32_t)b[2]<<8 | b[3];
        TEST_ASSERT_EQUAL_UINT64(expected, provenance_value(scope, name));
    }
    TEST_ASSERT_EQUAL_UINT64(observed_creates[index].handle, provenance_value(scope, "shader.create.handle"));
    TEST_ASSERT_EQUAL_UINT64(1, provenance_value(scope, "shader.create.cpu_returned"));
    TEST_ASSERT_EQUAL_UINT32(index+1, observed_creates[index].hashed_before);
    return scope;
}
static void provenance_link(unsigned index, uint64_t vs, uint64_t fs, unsigned reason)
{
    const uint64_t scope = provenance_scope("renderer.shader.load_link", index);
    TEST_ASSERT_EQUAL_UINT64(jce_trace_capture_generation(), provenance_value(scope, "shader.owner.capture"));
    TEST_ASSERT_EQUAL_UINT64(vs >> 32, provenance_value(scope, "shader.owner.vs_create_hi"));
    TEST_ASSERT_EQUAL_UINT64((uint32_t)vs, provenance_value(scope, "shader.owner.vs_create_lo"));
    TEST_ASSERT_EQUAL_UINT64(fs >> 32, provenance_value(scope, "shader.owner.fs_create_hi"));
    TEST_ASSERT_EQUAL_UINT64((uint32_t)fs, provenance_value(scope, "shader.owner.fs_create_lo"));
    TEST_ASSERT_EQUAL_UINT64(reason, provenance_value(scope, "shader.owner.vs_reason"));
    TEST_ASSERT_EQUAL_UINT64(reason, provenance_value(scope, "shader.owner.fs_reason"));
    TEST_ASSERT_EQUAL_UINT64(1, provenance_value(scope, "shader.owner.cpu_returned"));
    const uint64_t child = provenance_scope("renderer.shader.link", index);
    TEST_ASSERT_EQUAL_UINT64(2, provenance_value(child, "shader.link.creator_releases"));
    unsigned fields = 0;
    for (unsigned i = 0; i < provenance_count; ++i)
        if (provenance_events[i].type == JCE_TRACE_EVENT_COUNTER && provenance_events[i].id == child) {
            TEST_ASSERT_EQUAL_UINT64(scope, provenance_events[i].parent_id); ++fields;
        }
    TEST_ASSERT_EQUAL_UINT32(6, fields);
}
static void test_eager_payload_link_and_cached_acquires_have_distinct_receipts(void)
{
    start_provenance();
    programs[0] = shader_load_program_named(NULL, "grass", "grass_shadow");
    programs[1] = shader_load_program_named(NULL, "grass", "grass_shadow");
    TEST_ASSERT_TRUE(jce_shader_valid(programs[0]));
    TEST_ASSERT_EQUAL_UINT16(programs[0].idx, programs[1].idx);
    TEST_ASSERT_EQUAL_UINT32(4, observed_create_count);
    TEST_ASSERT_EQUAL_UINT32(4, sha_calls);
    read_provenance();
    uint64_t ids[4];
    for (unsigned i = 0; i < 4; ++i) ids[i] = provenance_create(i);
    TEST_ASSERT_NOT_EQUAL_UINT64(ids[0], ids[2]);
    TEST_ASSERT_NOT_EQUAL_UINT64(ids[1], ids[3]);
    provenance_link(0, ids[0], ids[1], 0); provenance_link(1, ids[2], ids[3], 0);
    expect_link_trace(2, JCE_TRACE_TASK_SUCCEEDED, 0);
    for (unsigned i = 0; i < 2; ++i) {
        jce_shader_program_destroy(programs[i]); programs[i] = JCE_INVALID_SHADER;
    }
    baseline_restored();
}
static void test_eager_mismatch_retains_payloads_and_actual_failed_link_cleanup(void)
{
    start_provenance();
    programs[0] = shader_load_program_named(NULL, "grass", "shadow");
    TEST_ASSERT_FALSE(jce_shader_valid(programs[0]));
    read_provenance(); provenance_link(0, provenance_create(0), provenance_create(1), 0);
    expect_link_trace(1, JCE_TRACE_TASK_FAILED, 2); baseline_restored();
}
static void test_existing_empty_overlay_records_actual_partial_creator_release(void)
{
    start_provenance();
    TEST_ASSERT_TRUE(jce_fs_host_write_all(overlay_file, "", 0));
    programs[0] = shader_load_program_overlay_named(overlay_directory, NULL, "color", "color");
    TEST_ASSERT_FALSE(jce_shader_valid(programs[0]));
    TEST_ASSERT_EQUAL_UINT32(1, observed_create_count);
    read_provenance(); const uint64_t create = provenance_create(0);
    const uint64_t release = provenance_scope("renderer.shader.creator_release", 0);
    TEST_ASSERT_EQUAL_UINT64(create >> 32, provenance_value(release, "shader.release.create_hi"));
    TEST_ASSERT_EQUAL_UINT64((uint32_t)create, provenance_value(release, "shader.release.create_lo"));
    TEST_ASSERT_EQUAL_UINT64(0, provenance_value(release, "shader.release.reason"));
    TEST_ASSERT_EQUAL_UINT64(observed_creates[0].handle, provenance_value(release, "shader.release.handle"));
    TEST_ASSERT_EQUAL_UINT64(1, provenance_value(release, "shader.release.cpu_returned"));
    expect_link_trace(0, JCE_TRACE_TASK_SUCCEEDED, 0); baseline_restored();
}
static void test_actual_fragment_overlay_payload_is_observed_before_api_consumption(void)
{
    start_provenance(); write_actual_fragment_overlay();
    programs[0] = shader_load_program_overlay_named(overlay_directory, NULL, "color", "color");
    TEST_ASSERT_TRUE(jce_shader_valid(programs[0])); TEST_ASSERT_EQUAL_UINT32(1, overlay_reads);
    read_provenance(); provenance_link(0, provenance_create(0), provenance_create(1), 0);
    jce_shader_program_destroy(programs[0]); programs[0] = JCE_INVALID_SHADER; baseline_restored();
}
static void test_actual_device_latch_off_has_no_new_hash_clock_or_provenance(void)
{
    jce_trace_set_enabled(false); jce_renderer_destroy(renderer); renderer = NULL;
    set_native_diag_environment("0");
    create_synchronous_noop(); TEST_ASSERT_NOT_NULL(renderer);
    baseline = counts(); TEST_ASSERT_FALSE(jce_renderer_shader_provenance_policy_internal());
    /* Environment changes do not rewrite the admitted device's actual latch. */
    set_native_diag_environment("1");
    TEST_ASSERT_FALSE(jce_renderer_shader_provenance_policy_internal());
    jce_trace_reset(); jce_trace_set_enabled(true);
    sha_calls = shader_clock_calls = create_api_calls = 0;
    char key[96]; snprintf(key, sizeof key, "shaders/vs_grass_%s.bin", profile);
    JceShaderAcquireEvidence receipt = {0};
    stages[0] = load_single(stock, key, &receipt);
    TEST_ASSERT_NOT_EQUAL_UINT16(UINT16_MAX, stages[0].idx);
    TEST_ASSERT_EQUAL_UINT64(0, receipt.create_id);
    TEST_ASSERT_EQUAL_UINT32(1, create_api_calls);
    TEST_ASSERT_EQUAL_UINT32(0, sha_calls); TEST_ASSERT_EQUAL_UINT32(0, shader_clock_calls);
    read_provenance(); expect_no_new_provenance();
}
static void test_trace_off_and_thread_suppressed_skip_new_evidence(void)
{
    start_provenance(); observe_payloads = false;
    char key[96]; snprintf(key, sizeof key, "shaders/vs_grass_%s.bin", profile);
    JceShaderAcquireEvidence receipt = {0};
    jce_trace_set_enabled(false);
    stages[0] = load_single(stock, key, &receipt);
    TEST_ASSERT_EQUAL_UINT64(0, receipt.create_id);
    jce_trace_set_enabled(true);
    TEST_ASSERT_TRUE(jce_trace_suppress_begin());
    stages[1] = load_single(stock, key, &receipt);
    TEST_ASSERT_TRUE(jce_trace_suppress_end());
    TEST_ASSERT_EQUAL_UINT64(0, receipt.create_id);
    TEST_ASSERT_EQUAL_UINT32(2, create_api_calls);
    TEST_ASSERT_EQUAL_UINT32(0, sha_calls); TEST_ASSERT_EQUAL_UINT32(0, shader_clock_calls);
    read_provenance(); expect_no_new_provenance();
}
static void test_real_noop_create_rejects_magic_with_payload_and_no_fictitious_link(void)
{
    start_provenance(); write_fragment_overlay_variant(true);
    programs[0] = shader_load_program_overlay_named(overlay_directory, NULL, "color", "color");
    TEST_ASSERT_FALSE(jce_shader_valid(programs[0]));
    TEST_ASSERT_EQUAL_UINT32(2, observed_create_count);
    TEST_ASSERT_EQUAL_UINT32(2, sha_calls);
    TEST_ASSERT_NOT_EQUAL_UINT16(UINT16_MAX, observed_creates[0].handle);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, observed_creates[1].handle);
    TEST_ASSERT_EQUAL_UINT32(1, overlay_reads);
    read_provenance();
    const uint64_t vertex = provenance_create(0), rejected = provenance_create(1);
    unsigned failed_create = 0, link_scopes = 0;
    for (unsigned i = 0; i < provenance_count; ++i) {
        const JceTraceEvent *event = &provenance_events[i];
        if (event->type != JCE_TRACE_EVENT_TASK_END) continue;
        if (event->id == rejected) {
            TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_TASK_FAILED, event->value_u32[0]); ++failed_create;
        }
        if (strcmp(event->name, "renderer.shader.load_link") == 0) ++link_scopes;
    }
    TEST_ASSERT_EQUAL_UINT32(1, failed_create);
    TEST_ASSERT_EQUAL_UINT32(0, link_scopes);
    const uint64_t release = provenance_scope("renderer.shader.creator_release", 0);
    TEST_ASSERT_EQUAL_UINT64(vertex >> 32, provenance_value(release, "shader.release.create_hi"));
    TEST_ASSERT_EQUAL_UINT64((uint32_t)vertex, provenance_value(release, "shader.release.create_lo"));
    TEST_ASSERT_EQUAL_UINT64(0, provenance_value(release, "shader.release.reason"));
    TEST_ASSERT_EQUAL_UINT64(observed_creates[0].handle, provenance_value(release, "shader.release.handle"));
    TEST_ASSERT_EQUAL_UINT64(1, provenance_value(release, "shader.release.cpu_returned"));
    expect_link_trace(0, JCE_TRACE_TASK_FAILED, 0);
    baseline_restored();
}
static void test_capture_started_after_real_stages_keeps_absent_receipts_unknown(void)
{
    start_provenance(); observe_payloads = false;
    jce_trace_set_enabled(false); enable_trace_after_create = 2;
    programs[0] = shader_load_program_named(NULL, "grass", "grass_shadow");
    enable_trace_after_create = 0;
    TEST_ASSERT_TRUE(jce_shader_valid(programs[0]));
    TEST_ASSERT_TRUE(jce_trace_enabled());
    TEST_ASSERT_EQUAL_UINT32(2, create_api_calls);
    TEST_ASSERT_EQUAL_UINT32(0, sha_calls);
    read_provenance(); provenance_link(0, 0, 0, 1);
    unsigned creates = 0;
    for (unsigned i = 0; i < provenance_count; ++i)
        if (provenance_events[i].type == JCE_TRACE_EVENT_TASK_END &&
            strcmp(provenance_events[i].name, "renderer.shader.create") == 0) ++creates;
    TEST_ASSERT_EQUAL_UINT32(0, creates);
    jce_shader_program_destroy(programs[0]); programs[0] = JCE_INVALID_SHADER;
    baseline_restored();
}
static void test_capture_change_never_joins_old_create_ids_to_new_link(void)
{
    start_provenance();
    char key[96]; JceShaderAcquireEvidence receipts[2] = {{0},{0}};
    snprintf(key, sizeof key, "shaders/vs_grass_%s.bin", profile);
    stages[0] = load_single(stock, key, &receipts[0]);
    snprintf(key, sizeof key, "shaders/fs_grass_shadow_%s.bin", profile);
    stages[1] = load_single(stock, key, &receipts[1]);
    TEST_ASSERT_NOT_EQUAL_UINT64(0, receipts[0].create_id);
    const uint32_t old_generation = receipts[0].capture_generation;
    (void)counts(); /* Real Noop delivery is quiescent before the trace reset. */
    jce_trace_set_enabled(false); jce_trace_reset(); jce_trace_set_enabled(true);
    TEST_ASSERT_NOT_EQUAL_UINT32(old_generation, jce_trace_capture_generation());
    programs[0] = (JceShaderHandle){link_program_evidenced(stages[0], stages[1], &receipts[0], &receipts[1]).idx};
    stages[0].idx = stages[1].idx = UINT16_MAX;
    TEST_ASSERT_TRUE(jce_shader_valid(programs[0]));
    read_provenance(); provenance_link(0, 0, 0, 2);
    jce_shader_program_destroy(programs[0]); programs[0] = JCE_INVALID_SHADER; baseline_restored();
}

#endif /* Native-only provenance controls; existing Web/Noop cases remain. */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_actual_matching_stock_pair_retains_program_until_public_destroy);
    RUN_TEST(test_actual_mismatched_stock_pair_rejects_and_restores_all_baselines);
    RUN_TEST(test_public_blob_pair_links_and_releases);
    RUN_TEST(test_public_blob_pair_releases_mismatched_stages);
    RUN_TEST(test_actual_cached_program_has_two_independent_acquires);
    RUN_TEST(test_actual_trace_off_keeps_ownership_and_emits_no_link_records);
    RUN_TEST(test_fragment_only_stock_overlay_uses_embedded_vertex_without_missing_errors);
    RUN_TEST(test_existing_empty_stock_overlay_is_not_replaced_by_baseline);
    RUN_TEST(test_null_and_empty_stock_overlay_preserve_cached_acquires_and_aliases);
    RUN_TEST(test_startup_unbound_shader_load_uses_pak_without_overlay);
    RUN_TEST(test_startup_null_root_uses_pak_without_overlay);
    RUN_TEST(test_startup_empty_root_uses_pak_without_overlay);
    RUN_TEST(test_startup_owned_root_reads_fragment_and_preserves_named_stage_pair);
    RUN_TEST(test_startup_existing_empty_overlay_fails_closed_and_releases_creator);
    RUN_TEST(test_startup_existing_bad_overlay_fails_closed_and_releases_creators);
    RUN_TEST(test_new_renderer_after_destroy_does_not_inherit_startup_root);
#if !JCE_PLATFORM_WEB
    RUN_TEST(test_eager_payload_link_and_cached_acquires_have_distinct_receipts);
    RUN_TEST(test_eager_mismatch_retains_payloads_and_actual_failed_link_cleanup);
    RUN_TEST(test_existing_empty_overlay_records_actual_partial_creator_release);
    RUN_TEST(test_actual_fragment_overlay_payload_is_observed_before_api_consumption);
    RUN_TEST(test_actual_device_latch_off_has_no_new_hash_clock_or_provenance);
    RUN_TEST(test_trace_off_and_thread_suppressed_skip_new_evidence);
    RUN_TEST(test_real_noop_create_rejects_magic_with_payload_and_no_fictitious_link);
    RUN_TEST(test_capture_started_after_real_stages_keeps_absent_receipts_unknown);
    RUN_TEST(test_capture_change_never_joins_old_create_ids_to_new_link);
#endif
    return UNITY_END();
}
