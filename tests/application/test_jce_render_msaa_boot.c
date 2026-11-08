/* Complete RP/legacy parsers, actual stock boot blocks in source order,
 * actual Engine graphics owner and actual renderer MSAA setter. CPU only. */
#include "unity.h"
#include <jce/application/jce_app_interface.h>
#include <jce/os/core/jce_config.h>
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_renderer_caps.h>
#include <jce/renderer/jce_render_pipeline.h>
#include <jce/renderer/jce_render_settings.h>
#include <jce/resource/jce_archive_writer.h>
#include <jce/resource/jce_pak_loader.h>
#include <bgfx/c99/bgfx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct JceRenderer { bool is_fallback; uint32_t reset_flags; };
static JceRenderer renderer;
static bgfx_stats_t stats;
static unsigned reset_calls;
static uint32_t reset_flags;
static JceRenderSettings s_render_settings;
static bool s_render_settings_loaded;
typedef struct FixtureProject { const char *cooked_assets; } FixtureProject;
static FixtureProject *s_project;
static struct JcePakArchive *s_engine_pak;
static void *s_scene_renderer;
static void *pak_bytes;
static char packed_pipeline[512], packed_legacy[512];

JceRendererBackend jce_renderer_get_active_backend(void) { return JCE_BACKEND_D3D11; }
JceGpuTier jce_renderer_get_tier(void) { return JCE_GPU_TIER_HIGH; }
uint32_t jce_renderer_get_caps(void)
{ return JCE_CAP_TEXTURE_FLOAT | JCE_CAP_FRAMEBUFFER_FLOAT; }
JceRenderRecommendation jce_renderer_get_recommendation(void)
{
    JceRenderRecommendation value;
    memset(&value, 0, sizeof value);
    value.has_discrete_gpu = true;
    return value;
}
const char *jce_gpu_tier_name(JceGpuTier tier) { (void)tier; return "fixture"; }

static const bgfx_stats_t *stats_port(void) { return &stats; }
static void reset_port(int reason, uint32_t rw, uint32_t rh,
                       uint32_t width, uint32_t height, uint32_t flags)
{
    (void)reason; (void)rw; (void)rh;
    TEST_ASSERT_EQUAL_UINT(640, width);
    TEST_ASSERT_EQUAL_UINT(480, height);
    ++reset_calls;
    reset_flags = flags;
}
#define bgfx_get_stats stats_port
#define renderer_reset_cpu reset_port
#define JCE_RENDERER_RESET_MSAA 4
#include "setter.inc.h"
#undef renderer_reset_cpu
#undef bgfx_get_stats

#undef LOG_TAG
#define LOG_TAG "boot_msaa_fixture"
#include "graphics.inc.h"

static bool base_path_port(char *out, size_t capacity)
{
    int written = snprintf(out, capacity, "%s", JCE_TEST_MSAA_WORK_DIR);
    return written > 0 && (size_t)written < capacity;
}
#include "read-config.inc.h"
static void lod_port(float value) { (void)value; }
static void vsync_port(JceRenderer *value, bool enabled) { (void)value; (void)enabled; }
static void grass_port(void *value, bool enabled) { (void)value; (void)enabled; }
static void texture_quality_port(int8_t value) { (void)value; }
static void integer_port(int value) { (void)value; }
static void particle_port(float value) { (void)value; }
#ifndef JCE_TEST_MSAA_PACKAGED
#define JCE_TEST_MSAA_PACKAGED 0
#endif
#define JCE_DEFAULT_MAIN_PACKAGED_ONLY JCE_TEST_MSAA_PACKAGED
#define JCE_PARTICLES_SOFT_FADE_DEFAULT 1.0f
#define jce_fs_host_get_base_path base_path_port
#define jce_lod_set_global_bias lod_port
#define jce_renderer_set_vsync vsync_port
#define jce_scene_renderer_set_grass_enabled grass_port
#define jce_texture_set_quality_mip_bias texture_quality_port
#define jce_texture_set_aniso_override integer_port
#define jce_engine_set_target_fps integer_port
#define jce_lighting_set_pixel_light_count integer_port
#define jce_particles_set_soft_fade_distance particle_port
#include "load-settings.inc.h"
static void stock_apply(const JceServices *svc)
{
#include JCE_TEST_MSAA_STOCK_OWNER
}

static const char pipeline_path[] = "Settings/RenderPipeline.rp.json";
static const char legacy_path[] = "resources/_cooked/render_settings.json";
static const char custom_legacy_path[] = "resources/custom/render_settings.json";
static void write_json(const char *path, const char *bytes)
{
    TEST_ASSERT_TRUE(jce_fs_host_write_all(path, bytes, (uint64_t)strlen(bytes)));
    char *packed = strcmp(path, pipeline_path) == 0 ? packed_pipeline : packed_legacy;
    TEST_ASSERT_LESS_THAN_UINT(512, strlen(bytes) + 1);
    strcpy(packed, bytes);
}
static JceConfig user_config(int samples)
{
    JceConfig cfg = jce_config_defaults();
    cfg.gfx_valid = true;
    cfg.gfx_quality = 4;
    cfg.gfx_msaa = samples;
    return cfg;
}
static void prepare_pak(void)
{
    if (s_engine_pak) jce_pak_close(s_engine_pak);
    jce_free(pak_bytes);
    s_engine_pak = NULL; pak_bytes = NULL;
    JceArchiveWriter *writer = jce_archive_writer_create(NULL);
    TEST_ASSERT_NOT_NULL(writer);
    if (packed_pipeline[0])
        TEST_ASSERT_TRUE(jce_archive_writer_add(writer, "settings/render_pipeline.rp.json",
            packed_pipeline, strlen(packed_pipeline)));
    if (packed_legacy[0])
        TEST_ASSERT_TRUE(jce_archive_writer_add(writer, "render_settings.json",
            packed_legacy, strlen(packed_legacy)));
    size_t pak_size = 0;
    TEST_ASSERT_TRUE(jce_archive_writer_finish(writer, &pak_bytes, &pak_size));
    jce_archive_writer_destroy(writer);
    s_engine_pak = jce_pak_open(pak_bytes, pak_size);
    TEST_ASSERT_NOT_NULL(s_engine_pak);
}
static void apply(const JceConfig *cfg)
{
    if (JCE_TEST_MSAA_PACKAGED) prepare_pak();
    JceServices services;
    memset(&services, 0, sizeof services);
    services.config = cfg;
    services.renderer = &renderer;
    stock_apply(&services);
}
static void assert_samples(int requested, int effective)
{
    JceRenderPipelineDesc desc;
    jce_render_pipeline_get_requested(&desc);
    TEST_ASSERT_EQUAL_INT_MESSAGE(requested, desc.msaa_samples,
        "Shared RP requested MSAA must retain the highest explicit configuration layer");
    jce_render_pipeline_get(&desc);
    TEST_ASSERT_EQUAL_INT_MESSAGE(effective, desc.msaa_samples,
        "Shared RP active MSAA must include the final effective policy");
    uint32_t expected = BGFX_RESET_VSYNC;
    if (effective == 2) expected |= BGFX_RESET_MSAA_X2;
    if (effective == 4) expected |= BGFX_RESET_MSAA_X4;
    if (effective == 8) expected |= BGFX_RESET_MSAA_X8;
    if (effective == 16) expected |= BGFX_RESET_MSAA_X16;
    TEST_ASSERT_EQUAL_UINT_MESSAGE(expected, renderer.reset_flags,
        "First boot must synchronize renderer flags with effective RP MSAA and retain VSync");
    if (reset_calls) TEST_ASSERT_EQUAL_UINT(renderer.reset_flags, reset_flags);
}
void setUp(void)
{
    remove(pipeline_path);
    remove(legacy_path);
    remove(custom_legacy_path);
    memset(&renderer, 0, sizeof renderer);
    renderer.reset_flags = BGFX_RESET_VSYNC;
    memset(&stats, 0, sizeof stats);
    stats.width = 640; stats.height = 480;
    reset_calls = 0; reset_flags = 0;
    s_render_settings_loaded = false;
    memset(&s_render_settings, 0, sizeof s_render_settings);
    s_project = NULL; s_engine_pak = NULL; s_scene_renderer = NULL;
    pak_bytes = NULL;
    packed_pipeline[0] = packed_legacy[0] = '\0';
    JceRenderPipelineDesc baseline;
    jce_render_pipeline_preset_for_current_tier(&baseline);
    jce_render_pipeline_apply(&baseline);
}
void tearDown(void)
{
    if (s_engine_pak) jce_pak_close(s_engine_pak);
    jce_free(pak_bytes);
    s_engine_pak = NULL; pak_bytes = NULL;
    remove(pipeline_path); remove(legacy_path);
    remove(custom_legacy_path);
}

static void test_explicit_canonical_off_outranks_legacy_two(void)
{
    write_json(pipeline_path, "{\"msaa_samples\":1}");
    write_json(legacy_path, "{\"msaa\":2}");
    apply(NULL);
    assert_samples(1, 1);
}
static void test_user_graphics_outranks_canonical_and_legacy(void)
{
    write_json(pipeline_path, "{\"msaa_samples\":1}");
    write_json(legacy_path, "{\"msaa\":2}");
    JceConfig cfg = user_config(8);
    apply(&cfg);
    assert_samples(8, 8);
}
static void test_canonical_msaa_is_synchronized_on_first_apply_without_graphics(void)
{
    write_json(pipeline_path, "{\"msaa_samples\":4}");
    apply(NULL);
    assert_samples(4, 4);
    TEST_ASSERT_EQUAL_UINT(1, reset_calls);
}
static void test_legacy_msaa_is_fallback_when_pipeline_is_absent(void)
{
    write_json(legacy_path, "{\"msaa\":4}");
    apply(NULL);
    assert_samples(4, 4);
}
static void test_legacy_msaa_is_fallback_when_canonical_field_is_absent(void)
{
    write_json(pipeline_path, "{\"enable_bloom\":false}");
    write_json(legacy_path, "{\"msaa\":8}");
    apply(NULL);
    assert_samples(8, 8);
    JceRenderPipelineDesc desc;
    jce_render_pipeline_get(&desc);
    TEST_ASSERT_FALSE(desc.enable_bloom);
}
static void test_legacy_missing_msaa_preserves_existing_fallback_off(void)
{
    write_json(legacy_path, "{\"lod_bias\":1.0}");
    apply(NULL);
    assert_samples(1, 1);
}
static void test_legacy_sixteen_samples_survives_a_missing_canonical_field(void)
{
    write_json(pipeline_path, "{\"enable_bloom\":false}");
    write_json(legacy_path, "{\"msaa\":16}");
    apply(NULL);
    assert_samples(16, 16);
}
static void test_public_load_still_defaults_missing_fields_to_low(void)
{
    write_json(pipeline_path, "{\"enable_bloom\":false}");
    write_json(legacy_path, "{\"msaa\":16}");
    apply(NULL);
    assert_samples(16, 16);
    JceRenderPipelineDesc loaded;
    TEST_ASSERT_TRUE(jce_render_pipeline_load(pipeline_path, &loaded));
    TEST_ASSERT_EQUAL_UINT(1, loaded.msaa_samples);
    if (s_engine_pak) {
        TEST_ASSERT_TRUE(jce_render_pipeline_load_pak(s_engine_pak,
            "settings/render_pipeline.rp.json", &loaded));
        TEST_ASSERT_EQUAL_UINT(1, loaded.msaa_samples);
    }
}
static void test_user_legal_samples_and_invalid_request_normalization(void)
{
    static const int requested[] = {1, 2, 4, 8, 3};
    static const int expected[] = {1, 2, 4, 8, 1};
    write_json(pipeline_path, "{\"msaa_samples\":1}");
    write_json(legacy_path, "{\"msaa\":2}");
    for (unsigned i = 0; i < sizeof requested / sizeof requested[0]; ++i) {
        JceConfig cfg = user_config(requested[i]);
        apply(&cfg);
        assert_samples(expected[i], expected[i]);
    }
}
static void test_engine_without_graphics_preserves_its_strict_noop_contract(void)
{
    JceRenderPipelineDesc desc;
    jce_render_pipeline_preset_high(&desc);
    desc.msaa_samples = 4;
    jce_render_pipeline_apply(&desc);
    JceConfig cfg = jce_config_defaults();
    TEST_ASSERT_FALSE(cfg.gfx_valid);
    jce_engine_apply_graphics_config(&cfg, &renderer);
    jce_engine_apply_graphics_config(NULL, &renderer);
    jce_render_pipeline_get_requested(&desc);
    TEST_ASSERT_EQUAL_UINT(4, desc.msaa_samples);
    TEST_ASSERT_EQUAL_UINT(BGFX_RESET_VSYNC, renderer.reset_flags);
    TEST_ASSERT_EQUAL_UINT(0, reset_calls);
}
static void test_normal_legacy_load_uses_the_project_custom_cooked_path(void)
{
    FixtureProject project = {"resources/custom"};
    s_project = &project;
    write_json(pipeline_path, "{\"enable_bloom\":false}");
    write_json(legacy_path, "{\"msaa\":2}");
    const char *custom = "{\"msaa\":4}";
    TEST_ASSERT_TRUE(jce_fs_host_write_all(custom_legacy_path, custom,
        (uint64_t)strlen(custom)));
    apply(NULL);
    TEST_ASSERT_TRUE(s_render_settings_loaded);
    TEST_ASSERT_EQUAL_INT(4, s_render_settings.msaa);
    assert_samples(4, 4);
}
static void test_normal_bad_host_legacy_falls_back_to_real_pak(void)
{
    write_json(pipeline_path, "{\"enable_bloom\":false}");
    write_json(legacy_path, "{\"msaa\":4}");
    prepare_pak();
    const char *invalid = "{broken";
    TEST_ASSERT_TRUE(jce_fs_host_write_all(legacy_path, invalid,
        (uint64_t)strlen(invalid)));
    apply(NULL);
    TEST_ASSERT_TRUE(s_render_settings_loaded);
    TEST_ASSERT_EQUAL_INT(4, s_render_settings.msaa);
    assert_samples(4, 4);
}
static void test_packaged_policy_ignores_conflicting_loose_developer_files(void)
{
    write_json(pipeline_path, "{\"msaa_samples\":1}");
    write_json(legacy_path, "{\"msaa\":2}");
    const char *foreign_pipeline = "{\"msaa_samples\":8}";
    const char *foreign_legacy = "{\"msaa\":16}";
    TEST_ASSERT_TRUE(jce_fs_host_write_all(pipeline_path, foreign_pipeline,
        (uint64_t)strlen(foreign_pipeline)));
    TEST_ASSERT_TRUE(jce_fs_host_write_all(legacy_path, foreign_legacy,
        (uint64_t)strlen(foreign_legacy)));
    apply(NULL);
    assert_samples(1, 1);
}
static void test_fallback_renderer_retains_requested_policy_without_reset(void)
{
    write_json(pipeline_path, "{\"msaa_samples\":4}");
    renderer.is_fallback = true;
    apply(NULL);
    JceRenderPipelineDesc desc;
    jce_render_pipeline_get(&desc);
    TEST_ASSERT_EQUAL_UINT(4, desc.msaa_samples);
    TEST_ASSERT_EQUAL_UINT(BGFX_RESET_VSYNC, renderer.reset_flags);
    TEST_ASSERT_EQUAL_UINT(0, reset_calls);
}
static void test_direct_renderer_msaa_remains_available_after_boot(void)
{
    write_json(pipeline_path, "{\"msaa_samples\":1}");
    apply(NULL);
    jce_renderer_set_msaa(&renderer, 8);
    TEST_ASSERT_EQUAL_UINT(BGFX_RESET_VSYNC | BGFX_RESET_MSAA_X8, renderer.reset_flags);
    jce_renderer_set_msaa(&renderer, 8);
    TEST_ASSERT_EQUAL_UINT(1, reset_calls);
    jce_renderer_set_msaa(&renderer, 0);
    TEST_ASSERT_EQUAL_UINT(BGFX_RESET_VSYNC, renderer.reset_flags);
    TEST_ASSERT_EQUAL_UINT(2, reset_calls);
}
static void test_force_outranks_user_and_renderer_consumes_effective_policy(void)
{
    write_json(pipeline_path, "{\"msaa_samples\":1}");
    JceConfig cfg = user_config(8);
    apply(&cfg);
    assert_samples(8, 4);
}
static void test_force_without_graphics_reaches_stock_final_sync(void)
{
    write_json(pipeline_path, "{\"msaa_samples\":1}");
    write_json(legacy_path, "{\"msaa\":2}");
    apply(NULL);
    assert_samples(1, 4);
    TEST_ASSERT_EQUAL_UINT(1, reset_calls);
}
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    UNITY_BEGIN();
    if (argc > 1 && strcmp(argv[1], "force") == 0) {
        RUN_TEST(test_force_outranks_user_and_renderer_consumes_effective_policy);
        RUN_TEST(test_force_without_graphics_reaches_stock_final_sync);
    } else {
        RUN_TEST(test_explicit_canonical_off_outranks_legacy_two);
        RUN_TEST(test_user_graphics_outranks_canonical_and_legacy);
        RUN_TEST(test_canonical_msaa_is_synchronized_on_first_apply_without_graphics);
        RUN_TEST(test_legacy_msaa_is_fallback_when_pipeline_is_absent);
        RUN_TEST(test_legacy_msaa_is_fallback_when_canonical_field_is_absent);
        RUN_TEST(test_legacy_missing_msaa_preserves_existing_fallback_off);
        RUN_TEST(test_legacy_sixteen_samples_survives_a_missing_canonical_field);
        RUN_TEST(test_public_load_still_defaults_missing_fields_to_low);
        RUN_TEST(test_user_legal_samples_and_invalid_request_normalization);
        RUN_TEST(test_engine_without_graphics_preserves_its_strict_noop_contract);
        if (!JCE_TEST_MSAA_PACKAGED) {
            RUN_TEST(test_normal_legacy_load_uses_the_project_custom_cooked_path);
            RUN_TEST(test_normal_bad_host_legacy_falls_back_to_real_pak);
        }
        if (JCE_TEST_MSAA_PACKAGED)
            RUN_TEST(test_packaged_policy_ignores_conflicting_loose_developer_files);
        RUN_TEST(test_fallback_renderer_retains_requested_policy_without_reset);
        RUN_TEST(test_direct_renderer_msaa_remains_available_after_boot);
    }
    return UNITY_END();
}
