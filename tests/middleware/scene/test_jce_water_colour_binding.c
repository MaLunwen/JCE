/* Actual water admission/global-bind and terminal-bind/submit source slices.
 * Wave/FFT/ripple/resource preparation between those slices is outside scope.
 * Reuse the actual PBR/lighting bodies and typed named-handle ports from the
 * grass fixture; no GPU device, shader execution or water pixels are tested. */
#include "unity.h"
/* The included fixture undefines its owner macros before the grass body.
 * Keep the scene log tag available to that body's counted log call. */
#if !defined(JCE_DIST) && !defined(JCE_LOG_NONE)
static const char LOG_TAG[] = "scene_renderer";
#endif
#define setUp grass_binding_set_up
#define tearDown grass_binding_tear_down
#define main grass_binding_fixture_main
#include "test_jce_grass_colour_binding.c"
#define bgfx_set_texture counted_texture
#undef main
#undef tearDown
#undef setUp
#include <jce/renderer/jce_planar_reflection.h>

static JceWaterComponent water;
static unsigned char water_mesh_token, water_scene_token, water_planar_token;
static uint16_t water_vertex, water_index;
static unsigned global_binds, fft_cpu_reports;
static unsigned common_program_loads, physical_program_loads, program_warnings;
static bool refuse_physical_program;
static uint32_t water_layer(const JceScene *scene, JceEntity entity)
{ TEST_ASSERT_EQUAL_PTR(&water_scene_token, scene); TEST_ASSERT_EQUAL_UINT64(42u, entity); return 6u; }
static uint32_t water_vertex_handle(const JceMesh *mesh)
{ TEST_ASSERT_EQUAL_PTR(&water_mesh_token, mesh); return water_vertex; }
static uint32_t water_index_handle(const JceMesh *mesh)
{ TEST_ASSERT_EQUAL_PTR(&water_mesh_token, mesh); return water_index; }
static uint32_t water_vertex_count(const JceMesh *mesh)
{ TEST_ASSERT_EQUAL_PTR(&water_mesh_token, mesh); return 16u; }
static uint32_t water_index_count(const JceMesh *mesh)
{ TEST_ASSERT_EQUAL_PTR(&water_mesh_token, mesh); return 24u; }
static JceTextureHandle water_planar_texture(const JcePlanarReflection *planar)
{ TEST_ASSERT_EQUAL_PTR(&water_planar_token, planar); return (JceTextureHandle){78u}; }
static jce_mat4 water_planar_matrix(const JcePlanarReflection *planar)
{ TEST_ASSERT_EQUAL_PTR(&water_planar_token, planar); return jce_m4_identity(); }

/* The frame helper is a typed composition port. Its actual PBR/default and
 * complete light-upload bodies run; CSM/cloud/fog use the existing typed leaves.
 * IBL/probe/weather preparation is outside this focused ownership fixture. */
static void water_global(JceSceneRenderer *sr, const JcePbrMaterial *material,
    uint16_t view, JceScene *scene, EntityList *list, uint32_t layer)
{
    (void)scene; TEST_ASSERT_NOT_NULL(list); TEST_ASSERT_EQUAL_PTR(&renderer, sr);
    TEST_ASSERT_EQUAL_UINT16(24u, view); TEST_ASSERT_EQUAL_UINT32(6u, layer);
    TEST_ASSERT_NOT_NULL(material); ++global_binds;
    jce_pbr_material_bind(material, sr->renderer, view);
    jce_light_env_apply_for_layer(sr->light_env, sr->renderer, layer);
    sr_bind_frame_shadow_state(sr);
    float output[4]; sr_pack_colour_output(jce_texture_colour_space(), sr->postfx_tonemap_active, output);
    counted_uniform(sr->u_ibl_params, output, 1u);
    sr_apply_fog_uniforms(sr);
}

static void water_transient_vertex(uint8_t stream,
    const bgfx_transient_vertex_buffer_t *buffer, uint32_t start, uint32_t count)
{
    TEST_ASSERT_NOT_NULL(buffer);
    counted_vertex(stream, buffer->handle, start, count);
}

#ifndef JCE_LOG_NONE
static void water_cpu_log(JceLogLevel level, const char *tag, const char *file,
    int line, const char *format, ...)
{
    if (level == JCE_LOG_LEVEL_WARN) {
        TEST_ASSERT_EQUAL_STRING("water shader '%s' not found in PAK", format);
        va_list args;
        va_start(args, format);
        const char *key = va_arg(args, const char *);
        va_end(args);
        TEST_ASSERT_EQUAL_STRING("water_physical", key);
        ++program_warnings;
        return;
    }
    TEST_ASSERT_EQUAL_INT(JCE_LOG_LEVEL_INFO, level);
    TEST_ASSERT_EQUAL_STRING("scene_renderer", tag);
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_GREATER_THAN_INT(0, line);
    TEST_ASSERT_EQUAL_STRING(
        "water: compatibility FFT CPU surface submitted (sampled_vertices=%u)", format);
    va_list args;
    va_start(args, format);
    const unsigned sampled = va_arg(args, unsigned);
    va_end(args);
    TEST_ASSERT_EQUAL_UINT(16u, sampled);
    TEST_ASSERT_EQUAL_UINT(1u, draw_count);
    ++fft_cpu_reports;
}
#undef jce_log_write
#define jce_log_write water_cpu_log
#endif

#define bgfx_set_transient_vertex_buffer water_transient_vertex
#define sr_inline_bind_pbr_global water_global
#define jce_scene_get_entity_layer water_layer
#define jce_mesh_get_vbh water_vertex_handle
#define jce_mesh_get_ibh water_index_handle
#define jce_mesh_vertex_count water_vertex_count
#define jce_mesh_index_count water_index_count
#define jce_planar_reflection_texture water_planar_texture
#define jce_planar_reflection_view_proj water_planar_matrix

static JceShaderHandle water_load_program(const JcePakArchive *pak, const char *key)
{
    (void)pak;
    if (strcmp(key, "water") == 0) {
        ++common_program_loads;
        return (JceShaderHandle){19u};
    }
    TEST_ASSERT_EQUAL_STRING("water_physical", key);
    ++physical_program_loads;
    return (JceShaderHandle){refuse_physical_program ? UINT16_MAX : 20u};
}

#define shader_load_program water_load_program
#include JCE_TEST_WATER_PROGRAM
#undef shader_load_program

/* The focused fixture exercises the direct terminal path. GPU optics
 * admission, resource copies and resolve are covered by the native probe. */
static bgfx_program_handle_t water_optics_unavailable(JceSceneRenderer *sr, JceScene *scene,
    const JceCamera *camera, JceEntity entity)
{
    (void)sr; (void)scene; (void)camera; (void)entity;
    return (bgfx_program_handle_t){UINT16_MAX};
}
#define sr_water_optics_trace water_optics_unavailable

static bool legacy_water, legacy_fft_mesh;
static void water_emit(void)
{
    const bool legacy_gl21 = legacy_water;
    const bool compat_fft_vertices = legacy_fft_mesh;
    bgfx_transient_vertex_buffer_t fft_vertices = {0};
    fft_vertices.handle.idx = 79u;
    JceSceneRenderer *sr = &renderer;
    JceScene *scene = (JceScene *)&water_scene_token;
    EntityList storage = {NULL, 0, 0}; EntityList *list = &storage;
    const JceEntity e = 42u; const uint16_t view_id = 24u;
    JceMesh *mesh = (JceMesh *)&water_mesh_token;
    const JceWaterComponent *wc = &water;
    const JceCamera *camera = NULL;
    const int water_mode = wc->water_mode;
    const bool have_depth = false;
    bool legacy_physical;
    const bgfx_program_handle_t water_program =
        sr_water_select_program(sr, legacy_gl21, wc->water_mode, &legacy_physical);
    if (!BGFX_HANDLE_IS_VALID(water_program)) return;
#include JCE_TEST_WATER_BINDING_PREFIX
#include JCE_TEST_WATER_BINDING_TERMINAL
}

void setUp(void)
{
    grass_binding_set_up();
    memset(&water, 0, sizeof water); global_binds = 0u; fft_cpu_reports = 0u;
    legacy_water = false; legacy_fft_mesh = false; refuse_physical_program = false;
    common_program_loads = 0u; physical_program_loads = 0u; program_warnings = 0u;
    water_vertex = 21u; water_index = 22u;
    renderer.prog_water.idx = UINT16_MAX;
    renderer.prog_water_physical.idx = UINT16_MAX;
    renderer.s_water_planar = uniform_handle("s_water_planar", BGFX_UNIFORM_TYPE_SAMPLER, 1u);
    renderer.u_water_planar = uniform_handle("u_water_planar", BGFX_UNIFORM_TYPE_VEC4, 1u);
    renderer.u_water_planar_vp = uniform_handle("u_water_planar_vp", BGFX_UNIFORM_TYPE_MAT4, 1u);
    clear_writes();
}
void tearDown(void) { grass_binding_tear_down(); }
static void assert_water_draw(unsigned item, int mode, bool shadows)
{
    TEST_ASSERT_LESS_THAN_UINT(draw_count, item); DrawRecord *draw = &draws[item];
    const float normal[4] = {0, 0, (float)mode, 0};
    const float cam[4] = {100, 200, 300, 0};
    const float output[4] = {0, 4, 1.0f / 2.2f, 1};
    assert_uniform(draw, "u_normalScale", normal, 4u);
    assert_uniform(draw, "u_cameraPos", cam, 4u);
    assert_uniform(draw, "u_iblParams", output, 4u);
    assert_uniform(draw, "u_ambientColor", light_env.packed_ambient, 4u);
    assert_uniform(draw, "u_lightCounts", light_env.packed_counts, 4u);
    assert_uniform(draw, "u_fogParams", renderer.fog_frame.params, 4u);
    TEST_ASSERT_EQUAL_UINT(1u, named(draw->uniform, "u_csmSplits")->writes);
    TEST_ASSERT_EQUAL_FLOAT(shadows ? 12.0f : 0.0f,
        named(draw->uniform, "u_csmSplits")->value[0]);
    for (unsigned i = 0; i < 4u; ++i) {
        char name[32]; snprintf(name, sizeof name, "s_csmShadow%u", i);
        UniformRecord *record = named(draw->uniform, name);
        TEST_ASSERT_EQUAL_UINT(1u, record->writes); TEST_ASSERT_EQUAL_UINT8(9u + i, record->stage);
        TEST_ASSERT_EQUAL_UINT16(60u + i, record->texture);
    }
    const uint64_t expected = BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LESS |
        BGFX_STATE_BLEND_ALPHA | BGFX_STATE_MSAA | (water.depth_write ? BGFX_STATE_WRITE_Z : 0u);
    TEST_ASSERT_EQUAL_UINT64(expected, draw->state);
    TEST_ASSERT_EQUAL_UINT16(21u, draw->vertex); TEST_ASSERT_EQUAL_UINT16(22u, draw->index);
    TEST_ASSERT_EQUAL_UINT16(19u, draw->program); TEST_ASSERT_EQUAL_UINT16(24u, draw->view);
    UniformRecord *cloud = named(draw->uniform, "s_cloudShadow");
    TEST_ASSERT_EQUAL_UINT(1u, cloud->writes); TEST_ASSERT_EQUAL_UINT8(3u, cloud->stage);
    TEST_ASSERT_EQUAL_UINT16(71u, cloud->texture);
    UniformRecord *planar = named(draw->uniform, "s_water_planar");
    TEST_ASSERT_EQUAL_UINT(1u, planar->writes); TEST_ASSERT_EQUAL_UINT8(8u, planar->stage);
    TEST_ASSERT_EQUAL_UINT16(water.planar_reflection ? 78u : 72u, planar->texture);
    const float pp[4] = {water.planar_reflection ? 1.0f : 0.0f,
        water.planar_intensity > 0.0f ? water.planar_intensity : 1.0f, 0, 0};
    assert_uniform(draw, "u_water_planar", pp, 4u);
    const jce_mat4 identity = jce_m4_identity();
    assert_uniform(draw, "u_water_planar_vp", (const float *)&identity, 16u);
}
static void successive_water_slices_bind_shadow_and_view_mode_once(void)
{
    jce_pbr_material_set_view_mode(8); water_emit(); assert_water_draw(0u, 8, true);
    renderer.frame_shadow_active = false; water.depth_write = true;
    water.planar_reflection = true; water.planar_intensity = .65f;
    renderer.planar = (JcePlanarReflection *)&water_planar_token;
    jce_pbr_material_set_view_mode(10); water_emit(); assert_water_draw(1u, 10, false);
    TEST_ASSERT_EQUAL_UINT(2u, draw_count); TEST_ASSERT_EQUAL_UINT(2u, global_binds);
    TEST_ASSERT_EQUAL_UINT(2u, shadow_calls); TEST_ASSERT_EQUAL_UINT(2u, cloud_calls);
    assert_no_pending_writes();
}
static void invalid_geometry_rejects_before_the_first_draw_uniform(void)
{
    water_vertex = UINT16_MAX; water_emit();
    TEST_ASSERT_EQUAL_UINT(0u, global_binds); TEST_ASSERT_EQUAL_UINT(0u, draw_count);
    assert_no_pending_writes();
    water_vertex = 21u; water_index = UINT16_MAX; water_emit();
    TEST_ASSERT_EQUAL_UINT(0u, global_binds); TEST_ASSERT_EQUAL_UINT(0u, draw_count);
    assert_no_pending_writes();
}
static void rejected_water_does_not_seed_the_next_admitted_draw(void)
{
    water_index = UINT16_MAX; water_emit(); assert_no_pending_writes();
    water_index = 22u; jce_pbr_material_set_view_mode(2); water_emit();
    assert_water_draw(0u, 2, true); TEST_ASSERT_EQUAL_UINT(1u, global_binds);
    assert_no_pending_writes();
}
static void legacy_water_skips_physical_terminal_bindings(void)
{
    legacy_water = true;
    water.planar_reflection = true;
    renderer.planar = (JcePlanarReflection *)&water_planar_token;
    water_emit();
    TEST_ASSERT_EQUAL_UINT(1u, draw_count);
    TEST_ASSERT_EQUAL_UINT(1u, global_binds);
    TEST_ASSERT_EQUAL_UINT(0u, cloud_calls);
    TEST_ASSERT_EQUAL_UINT(0u, named(draws[0].uniform, "s_water_planar")->writes);
    TEST_ASSERT_EQUAL_UINT(0u, named(draws[0].uniform, "u_water_planar")->writes);
    assert_no_pending_writes();
}
static void legacy_fft_and_analytic_fallback_use_the_same_index_and_state(void)
{
    legacy_water = true;
    legacy_fft_mesh = true;
    water.water_mode = JCE_WATER_MODE_FFT;
    water.depth_write = true;
    water_emit();
    legacy_fft_mesh = false;
    water_emit();

    TEST_ASSERT_EQUAL_UINT(2u, draw_count);
    TEST_ASSERT_EQUAL_UINT(2u, global_binds);
    TEST_ASSERT_EQUAL_UINT16(79u, draws[0].vertex);
    TEST_ASSERT_EQUAL_UINT16(21u, draws[1].vertex);
    TEST_ASSERT_EQUAL_UINT16(22u, draws[0].index);
    TEST_ASSERT_EQUAL_UINT16(draws[0].index, draws[1].index);
    TEST_ASSERT_EQUAL_UINT16(20u, draws[0].program);
    TEST_ASSERT_EQUAL_UINT16(draws[0].program, draws[1].program);
    TEST_ASSERT_EQUAL_UINT64(draws[0].state, draws[1].state);
    TEST_ASSERT_TRUE((draws[0].state & BGFX_STATE_WRITE_Z) != 0u);
    legacy_fft_mesh = true;
    water_emit();
    TEST_ASSERT_EQUAL_UINT(3u, draw_count);
#ifndef JCE_LOG_NONE
    TEST_ASSERT_EQUAL_UINT(1u, fft_cpu_reports);
#endif
    assert_no_pending_writes();
}
static void water_program_cohorts_load_once_and_fail_independently(void)
{
    legacy_water = true;
    refuse_physical_program = true;
    water.water_mode = JCE_WATER_MODE_GERSTNER;
    water_emit();
    water.water_mode = JCE_WATER_MODE_FFT;
    water_emit();
    TEST_ASSERT_EQUAL_UINT(0u, draw_count);
    TEST_ASSERT_EQUAL_UINT(0u, global_binds);
    TEST_ASSERT_EQUAL_UINT(1u, physical_program_loads);
#ifndef JCE_LOG_NONE
    TEST_ASSERT_EQUAL_UINT(1u, program_warnings);
#endif
    assert_no_pending_writes();

    water.water_mode = JCE_WATER_MODE_STYLIZED;
    water_emit();
    legacy_water = false;
    water.water_mode = JCE_WATER_MODE_GERSTNER;
    water_emit();
    TEST_ASSERT_EQUAL_UINT(2u, draw_count);
    TEST_ASSERT_EQUAL_UINT16(19u, draws[0].program);
    TEST_ASSERT_EQUAL_UINT16(19u, draws[1].program);
    TEST_ASSERT_EQUAL_UINT(1u, common_program_loads);
    TEST_ASSERT_EQUAL_UINT(1u, physical_program_loads);
    assert_no_pending_writes();
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(successive_water_slices_bind_shadow_and_view_mode_once);
    RUN_TEST(invalid_geometry_rejects_before_the_first_draw_uniform);
    RUN_TEST(rejected_water_does_not_seed_the_next_admitted_draw);
    RUN_TEST(legacy_water_skips_physical_terminal_bindings);
    RUN_TEST(legacy_fft_and_analytic_fallback_use_the_same_index_and_state);
    RUN_TEST(water_program_cohorts_load_once_and_fail_independently);
    return UNITY_END();
}
