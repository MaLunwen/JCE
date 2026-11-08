/* Exact colour emission, geometry, directional lighting and view-mode owners.
 * Counted typed CPU ports model bgfx's uniform-name aliasing and ALL discard.
 * They establish command state/admission, never GPU completion or pixels. */
#include "unity.h"
#include "middleware/scene/jce_sr_grass_colour.h"
#include "renderer/jce_lighting_system_internal.h"
#include "renderer/jce_render_encoder.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define UNIFORM_CAP 96u
#define DRAW_CAP 16u
#define ROOT_CAP 24u
#define ROOT_STRIDE 80u
typedef struct UniformRecord {
    char name[40];
    bgfx_uniform_type_t type;
    uint16_t count;
    unsigned writes;
    float value[64];
    uint8_t stage;
    uint16_t texture;
    uint32_t flags;
} UniformRecord;
typedef struct DrawRecord {
    UniformRecord uniform[UNIFORM_CAP];
    uint32_t start, count;
    uint64_t state;
    uint16_t view, program, vertex, index;
} DrawRecord;
static UniformRecord uniforms[UNIFORM_CAP];
static DrawRecord draws[DRAW_CAP];
static unsigned uniform_count, draw_count, allocation_calls, availability_calls;
static unsigned short_allocation, empty_allocation, shadow_calls, cloud_calls;
static unsigned fog_calls, roots_calls;
static uint32_t batch_limit, capacity, bound_start, bound_count;
static uint16_t bound_vertex, bound_index;
static uint64_t bound_state;
static bool persist, roots_ready, linear_colour, expanded_grass;
static unsigned char packed[ROOT_CAP * ROOT_STRIDE], transient[ROOT_CAP * ROOT_STRIDE];
static JceSceneRenderer renderer;
static JceGrassFieldComponent field;
static jce_vec3 camera_position;
static const JceCamera *camera;
static const uint64_t expected_state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
    BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_MSAA;

static bgfx_uniform_handle_t uniform_handle(const char *name,
    bgfx_uniform_type_t type, uint16_t count)
{
    for (unsigned i = 0; i < uniform_count; ++i) {
        if (strcmp(uniforms[i].name, name) != 0) continue;
        TEST_ASSERT_EQUAL_INT(type, uniforms[i].type);
        return (bgfx_uniform_handle_t){(uint16_t)i};
    }
    TEST_ASSERT_LESS_THAN_UINT(UNIFORM_CAP, uniform_count);
    unsigned item = uniform_count++;
    snprintf(uniforms[item].name, sizeof uniforms[item].name, "%s", name);
    uniforms[item].type = type;
    uniforms[item].count = count;
    return (bgfx_uniform_handle_t){(uint16_t)item};
}
static UniformRecord *named(UniformRecord *records, const char *name)
{
    for (unsigned i = 0; i < uniform_count; ++i)
        if (strcmp(records[i].name, name) == 0) return &records[i];
    TEST_FAIL_MESSAGE("Expected named uniform was never acquired");
    return NULL;
}
static void clear_writes(void)
{
    for (unsigned i = 0; i < uniform_count; ++i) {
        uniforms[i].writes = 0u;
        memset(uniforms[i].value, 0xcd, sizeof uniforms[i].value);
    }
}
static void counted_uniform(bgfx_uniform_handle_t handle, const void *value, uint16_t count)
{
    TEST_ASSERT_LESS_THAN_UINT(uniform_count, handle.idx);
    UniformRecord *record = &uniforms[handle.idx];
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, record->writes, record->name);
    TEST_ASSERT_NOT_EQUAL(BGFX_UNIFORM_TYPE_SAMPLER, record->type);
    size_t floats = (size_t)count * (record->type == BGFX_UNIFORM_TYPE_MAT4 ? 16u : 4u);
    TEST_ASSERT_LESS_OR_EQUAL_UINT(sizeof record->value, floats * sizeof(float));
    memcpy(record->value, value, floats * sizeof(float));
    record->count = count;
    ++record->writes;
}
static void counted_texture(uint8_t stage, bgfx_uniform_handle_t sampler,
    bgfx_texture_handle_t texture, uint32_t flags)
{
    TEST_ASSERT_LESS_THAN_UINT(uniform_count, sampler.idx);
    UniformRecord *record = &uniforms[sampler.idx];
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, record->writes, record->name);
    TEST_ASSERT_EQUAL_INT(BGFX_UNIFORM_TYPE_SAMPLER, record->type);
    ++record->writes; /* setTexture itself writes the sampler uniform. */
    record->stage = stage; record->texture = texture.idx; record->flags = flags;
}
static void counted_log(JceLogLevel level, const char *tag, const char *file,
    int line, const char *format, ...)
{
    (void)level; (void)file; (void)line;
    va_list args; va_start(args, format);
    fprintf(stderr, "%s: ", tag); vfprintf(stderr, format, args); fputc('\n', stderr);
    va_end(args);
}
static const bgfx_memory_t *counted_copy(const void *data, uint32_t size)
{
    static unsigned char bytes[4];
    static bgfx_memory_t memory;
    TEST_ASSERT_LESS_OR_EQUAL_UINT(sizeof bytes, size);
    memcpy(bytes, data, size); memory.data = bytes; memory.size = size;
    return &memory;
}
static const bgfx_memory_t *counted_alloc(uint32_t size)
{ (void)size; TEST_FAIL_MESSAGE("Fixture must not allocate a real cookie atlas"); return NULL; }
static const bgfx_caps_t *counted_caps(void)
{ static bgfx_caps_t caps; return &caps; }
static bgfx_texture_handle_t counted_create_texture(uint16_t width, uint16_t height,
    bool mips, uint16_t layers, bgfx_texture_format_t format, uint64_t flags,
    const bgfx_memory_t *memory, uint64_t external)
{
    (void)mips; (void)format; (void)flags; (void)external;
    TEST_ASSERT_EQUAL_UINT16(1u, width); TEST_ASSERT_EQUAL_UINT16(1u, height);
    TEST_ASSERT_EQUAL_UINT16(1u, layers); TEST_ASSERT_NOT_NULL(memory);
    return (bgfx_texture_handle_t){50u};
}
static uint32_t counted_frame(const JceRenderer *unused)
{ (void)unused; return 17u; }
static void counted_blit(bgfx_view_id_t view, bgfx_texture_handle_t dst,
    uint8_t dst_mip, uint16_t dx, uint16_t dy, uint16_t dz,
    bgfx_texture_handle_t src, uint8_t src_mip, uint16_t sx, uint16_t sy,
    uint16_t sz, uint16_t width, uint16_t height, uint16_t depth)
{
    (void)view; (void)dst; (void)dst_mip; (void)dx; (void)dy; (void)dz;
    (void)src; (void)src_mip; (void)sx; (void)sy; (void)sz;
    (void)width; (void)height; (void)depth;
    TEST_FAIL_MESSAGE("Cookie atlas upload is outside this CPU fixture");
}
static uint32_t counted_available(uint32_t wanted, uint16_t stride)
{
    (void)wanted; TEST_ASSERT_EQUAL_UINT16(ROOT_STRIDE, stride); ++availability_calls;
    return capacity < batch_limit ? capacity : batch_limit;
}
static void counted_instance_alloc(bgfx_instance_data_buffer_t *data,
    uint32_t count, uint16_t stride)
{
    ++allocation_calls; memset(data, 0, sizeof *data);
    TEST_ASSERT_LESS_OR_EQUAL_UINT(ROOT_CAP, count);
    data->data = allocation_calls == empty_allocation ? NULL : transient;
    data->num = allocation_calls == short_allocation ? count - 1u : count;
    data->stride = stride;
}
static void counted_instances(const bgfx_instance_data_buffer_t *data,
    uint32_t start, uint32_t count)
{
    TEST_ASSERT_EQUAL_UINT32(0u, start); TEST_ASSERT_NOT_NULL(data->data);
    TEST_ASSERT_LESS_OR_EQUAL_UINT(capacity, count); capacity -= count;
    memcpy(&bound_start, data->data, sizeof bound_start); bound_count = count;
    TEST_ASSERT_EQUAL_MEMORY(packed + (size_t)bound_start * ROOT_STRIDE,
        data->data, (size_t)count * ROOT_STRIDE);
}
static void counted_resident(bgfx_dynamic_vertex_buffer_handle_t buffer,
    uint32_t start, uint32_t count)
{
    TEST_ASSERT_EQUAL_UINT16(29u, buffer.idx); bound_start = start; bound_count = count;
}
static void counted_vertex(uint8_t stream, bgfx_vertex_buffer_handle_t buffer,
    uint32_t start, uint32_t count)
{
    TEST_ASSERT_EQUAL_UINT8(0u, stream); TEST_ASSERT_EQUAL_UINT32(0u, start);
    TEST_ASSERT_EQUAL_UINT32(16u, count); bound_vertex = buffer.idx;
}
static void counted_index(bgfx_index_buffer_handle_t buffer, uint32_t start, uint32_t count)
{
    TEST_ASSERT_EQUAL_UINT32(0u, start); TEST_ASSERT_EQUAL_UINT32(24u, count);
    bound_index = buffer.idx;
}
static void counted_state(uint64_t state, uint32_t rgba)
{ TEST_ASSERT_EQUAL_UINT32(0u, rgba); bound_state = state; }
static void counted_submit(bgfx_view_id_t view, bgfx_program_handle_t program,
    uint32_t depth, uint8_t discard)
{
    TEST_ASSERT_EQUAL_UINT32(0u, depth); TEST_ASSERT_EQUAL_UINT8(BGFX_DISCARD_ALL, discard);
    TEST_ASSERT_LESS_THAN_UINT(DRAW_CAP, draw_count);
    DrawRecord *draw = &draws[draw_count++]; memcpy(draw->uniform, uniforms, sizeof uniforms);
    draw->start = bound_start; draw->count = bound_count; draw->state = bound_state;
    draw->view = view; draw->program = program.idx;
    draw->vertex = bound_vertex; draw->index = bound_index;
    clear_writes(); /* ALL removes the complete update range before the next emission. */
    bound_start = bound_count = bound_vertex = bound_index = 0u; bound_state = 0u;
}

/* Some Debug linkers retain references from the included PBR custom-program
 * paths. These typed ports make any accidental invocation an explicit failure;
 * the fixture never loads, decompresses or creates a custom shader program. */
static const JcePakAsset *unexpected_pak_find(const JcePakArchive *pak, const char *path)
{
    (void)pak; (void)path;
    TEST_FAIL_MESSAGE("Custom-program PAK lookup is outside this CPU fixture");
    return NULL;
}
static size_t unexpected_pak_decompress(const JcePakAsset *asset, void *buffer, size_t size)
{
    (void)asset; (void)buffer; (void)size;
    TEST_FAIL_MESSAGE("Custom-program PAK decompression is outside this CPU fixture");
    return 0u;
}
static JceShaderHandle unexpected_program_create(const void *vs, size_t vs_size,
    const void *fs, size_t fs_size)
{
    (void)vs; (void)vs_size; (void)fs; (void)fs_size;
    TEST_FAIL_MESSAGE("Custom-program creation is outside this CPU fixture");
    return JCE_INVALID_SHADER;
}
static void unexpected_program_destroy(JceShaderHandle program)
{
    (void)program;
    TEST_FAIL_MESSAGE("Custom-program destruction is outside this CPU fixture");
}
static const char *unexpected_backend_suffix(void)
{
    TEST_FAIL_MESSAGE("Custom-program backend selection is outside this CPU fixture");
    return NULL;
}
static const JcePakArchive *unexpected_engine_pak(void)
{
    TEST_FAIL_MESSAGE("Custom-program embedded PAK access is outside this CPU fixture");
    return NULL;
}

/* Compile the actual ordinary/directional light and PBR view-mode functions.
 * Only backend/clock/log ports are counted; selection and packing are real. */
static bgfx_renderer_type_t counted_backend(void)
{
    return BGFX_RENDERER_TYPE_OPENGL;
}

static JceRendererApiInfo counted_api_info(void)
{
    return (JceRendererApiInfo){
        .backend = JCE_BACKEND_OPENGL,
        .build_tier = JCE_GRAPHICS_API_TIER_STABLE,
    };
}

#define jce_enc_set_uniform counted_uniform
#define jce_enc_set_texture counted_texture
#define bgfx_set_uniform counted_uniform
#define bgfx_create_uniform uniform_handle
#define bgfx_copy counted_copy
#define bgfx_alloc counted_alloc
#define bgfx_get_caps counted_caps
#define bgfx_create_texture_2d counted_create_texture
#define bgfx_blit counted_blit
#define jce_renderer_get_frame_index counted_frame
#define jce_log_write counted_log
#define jce_pak_find unexpected_pak_find
#define jce_pak_decompress unexpected_pak_decompress
#define jce_renderer_create_program_from_blobs unexpected_program_create
#define jce_renderer_destroy_program unexpected_program_destroy
#define jce_shaders_backend_suffix unexpected_backend_suffix
#define jce_shaders_embedded_engine_pak unexpected_engine_pak
#undef LOG_TAG
#define bgfx_get_renderer_type counted_backend
#define jce_renderer_get_api_info counted_api_info
#include "renderer/jce_lighting_system.c"
#undef jce_renderer_get_api_info
#undef bgfx_get_renderer_type
#undef LOG_TAG
#include "renderer/jce_pbr_material.c"
#undef LOG_TAG
#define bgfx_get_avail_instance_data_buffer counted_available
#define bgfx_alloc_instance_data_buffer counted_instance_alloc
#define bgfx_set_instance_data_buffer counted_instances
#define bgfx_set_instance_data_from_dynamic_vertex_buffer counted_resident
#define bgfx_set_vertex_buffer counted_vertex
#define bgfx_set_index_buffer counted_index
#define bgfx_set_state counted_state
#define bgfx_submit counted_submit
#ifndef JCE_GRASS_COLOUR_BINDING_OWNER
#define JCE_GRASS_COLOUR_BINDING_OWNER "middleware/scene/jce_sr_grass_colour.c"
#endif
#define LOG_TAG "scene_renderer"
#include JCE_GRASS_COLOUR_BINDING_OWNER
#undef LOG_TAG

/* Shared scene binder leaves retain their existing owners. Their typed ports
 * publish representative complete active/disabled CSM, cloud and fog state. */
static const float shadow_params[4] = {.0005f, .001f, .2f, .4f};
void sr_bind_frame_shadow_state(JceSceneRenderer *sr)
{
    ++shadow_calls;
    const float splits[4] = {sr->frame_shadow_active ? 12.0f : 0.0f, 30, 90, 160};
    const jce_mat4 vp = jce_m4_identity();
    counted_uniform(sr->u_csm_vp, &vp, 1u);
    counted_uniform(sr->u_csm_splits, splits, 1u);
    counted_uniform(sr->u_csm_params, shadow_params, 1u);
    counted_uniform(sr->u_csm_bias_scales, shadow_params, 1u);
    for (unsigned i = 0; i < 4u; ++i)
        counted_texture((uint8_t)(9u + i), sr->u_csm_samplers[i], sr->csm_tex[i], UINT32_MAX);
}
void sr_bind_cloud_shadow(JceSceneRenderer *sr)
{
    ++cloud_calls;
    float cloud[4] = {0, 0, 0, 0};
    if (sr->cloud_shadow_valid) {
        cloud[0] = sr->cloud_shadow_extent;
        cloud[1] = sr->cloud_shadow_center[0];
        cloud[2] = sr->cloud_shadow_center[1];
        cloud[3] = 1.0f;
    }
    counted_texture(3u, sr->s_cloud_shadow,
        sr->cloud_shadow_valid ? sr->cloud_shadow_tex : sr->white_tex, UINT32_MAX);
    counted_uniform(sr->u_cloud_shadow, cloud, 1u);
}
void sr_apply_fog_uniforms(JceSceneRenderer *sr)
{
    ++fog_calls; counted_uniform(sr->u_fog_params, sr->fog_frame.params, 1u);
    if (sr->fog_frame.params[0] >= .5f) {
        counted_uniform(sr->u_fog_color, sr->fog_frame.color, 1u);
        counted_uniform(sr->u_fog_color_sun, sr->fog_frame.color_sun, 1u);
        counted_uniform(sr->u_fog_height, sr->fog_frame.height, 1u);
    }
}
bool sr_grass_persist_enabled(void) { return persist; }
bool sr_grass_expanded_mode(void) { return expanded_grass; }
bool sr_grass_expanded_bind(JceSceneRenderer *sr, int slot, uint32_t start, uint32_t count)
{
    TEST_ASSERT_EQUAL_PTR(&renderer, sr);
    TEST_ASSERT_EQUAL_INT(0, slot);
    if (start >= ROOT_CAP || count > ROOT_CAP - start) return false;
    bound_start = start; bound_count = count;
    bound_vertex = 21u; bound_index = 22u;
    return true;
}
bool sr_grass_roots_ensure(JceSceneRenderer *sr, int slot)
{ TEST_ASSERT_EQUAL_PTR(&renderer, sr); TEST_ASSERT_EQUAL_INT(0, slot); ++roots_calls; return roots_ready; }
jce_vec3 jce_camera_get_position(const JceCamera *value)
{ TEST_ASSERT_EQUAL_PTR(camera, value); return camera_position; }
float JCE_CALL jce_environment_wind_speed_now(const JceEnvironmentState *env)
{ (void)env; return 1.25f; }
int jce_texture_colour_space(void) { return linear_colour ? 1 : 0; }
jce_mat4 sr_foliage_instance_matrix(const JceFoliageInstance *instance, float align)
{ (void)instance; (void)align; return jce_m4_identity(); }

static JceLightEnv light_env;
void setUp(void)
{
    memset(&renderer, 0, sizeof renderer); memset(&field, 0, sizeof field);
    memset(&light_env, 0, sizeof light_env); memset(uniforms, 0, sizeof uniforms);
    memset(draws, 0, sizeof draws); memset(&s_lu, 0, sizeof s_lu);
    s_uniforms_init = false; uniform_count = draw_count = 0u;
    allocation_calls = availability_calls = short_allocation = empty_allocation = 0u;
    shadow_calls = cloud_calls = fog_calls = roots_calls = 0u;
    capacity = ROOT_CAP; batch_limit = 3u; persist = false; roots_ready = true;
    expanded_grass = false;
    bound_start = bound_count = bound_vertex = bound_index = 0u; bound_state = 0u;
    linear_colour = true; renderer.postfx_tonemap_active = true;
    camera_position = jce_v3(4.0f, 8.0f, -12.0f); camera = (const JceCamera *)&camera_position;
    renderer.grass_time = 6.75f; field.wind_dir[0] = .6f; field.wind_dir[1] = .8f;
    field.wind_speed = 2.0f; field.wind_amplitude = .35f;
    field.fade_start = 15.0f; field.fade_end = 27.0f; field.hue_jitter = .2f;
    for (unsigned i = 0; i < 3u; ++i) {
        field.root_color[i] = .1f * (float)(i + 1u); field.tip_color[i] = .2f * (float)(i + 1u);
    }
    renderer.grass_cache[0].packed = packed; renderer.grass_cache[0].inst_count = ROOT_CAP;
    renderer.grass_cache[0].inst_vb.idx = 29u; renderer.grass_cache[0].inst_vb_count = ROOT_CAP;
    for (uint32_t i = 0; i < ROOT_CAP; ++i) {
        memset(packed + i * ROOT_STRIDE, (int)(i + 1u), ROOT_STRIDE);
        memcpy(packed + i * ROOT_STRIDE, &i, sizeof i);
    }
#define BIND_HANDLE(member, name, type, count) renderer.member = uniform_handle(name, type, count)
    BIND_HANDLE(u_grass_time, "u_grass_time", BGFX_UNIFORM_TYPE_VEC4, 1u);
    BIND_HANDLE(u_grass_wind, "u_grass_wind", BGFX_UNIFORM_TYPE_VEC4, 1u);
    BIND_HANDLE(u_grass_fade, "u_grass_fade", BGFX_UNIFORM_TYPE_VEC4, 1u);
    BIND_HANDLE(u_grass_camera, "u_cameraPos", BGFX_UNIFORM_TYPE_VEC4, 1u);
    BIND_HANDLE(u_grass_color, "u_grass_color", BGFX_UNIFORM_TYPE_VEC4, 2u);
    BIND_HANDLE(u_ibl_params, "u_iblParams", BGFX_UNIFORM_TYPE_VEC4, 1u);
    BIND_HANDLE(u_csm_vp, "u_csmVP", BGFX_UNIFORM_TYPE_MAT4, 4u);
    BIND_HANDLE(u_csm_splits, "u_csmSplits", BGFX_UNIFORM_TYPE_VEC4, 1u);
    BIND_HANDLE(u_csm_params, "u_csmParams", BGFX_UNIFORM_TYPE_VEC4, 1u);
    BIND_HANDLE(u_csm_bias_scales, "u_csmBiasScales", BGFX_UNIFORM_TYPE_VEC4, 1u);
    BIND_HANDLE(s_cloud_shadow, "s_cloudShadow", BGFX_UNIFORM_TYPE_SAMPLER, 1u);
    BIND_HANDLE(u_cloud_shadow, "u_cloudShadow", BGFX_UNIFORM_TYPE_VEC4, 1u);
    BIND_HANDLE(u_fog_params, "u_fogParams", BGFX_UNIFORM_TYPE_VEC4, 1u);
    BIND_HANDLE(u_fog_color, "u_fogColor", BGFX_UNIFORM_TYPE_VEC4, 1u);
    BIND_HANDLE(u_fog_color_sun, "u_fogColorSun", BGFX_UNIFORM_TYPE_VEC4, 1u);
    BIND_HANDLE(u_fog_height, "u_fogHeight", BGFX_UNIFORM_TYPE_VEC4, 1u);
#undef BIND_HANDLE
    for (unsigned i = 0; i < 4u; ++i) {
        char name[32]; snprintf(name, sizeof name, "s_csmShadow%u", i);
        renderer.u_csm_samplers[i] = uniform_handle(name, BGFX_UNIFORM_TYPE_SAMPLER, 1u);
        renderer.csm_tex[i].idx = (uint16_t)(60u + i);
    }
    renderer.frame_shadow_active = true; renderer.cloud_shadow_valid = true;
    renderer.cloud_shadow_extent = 84.0f; renderer.cloud_shadow_center[0] = -6.0f;
    renderer.cloud_shadow_center[1] = 9.0f; renderer.cloud_shadow_tex.idx = 71u;
    renderer.white_tex.idx = 72u; renderer.fog_frame.params[0] = 2.0f;
    renderer.fog_frame.color[1] = .7f; renderer.fog_frame.color_sun[2] = .8f;
    renderer.fog_frame.height[3] = 4.0f;
    light_env.ambient_color = jce_v3(.2f, .3f, .4f); light_env.ambient_intensity = .75f;
    light_env.camera_pos = jce_v3(100.0f, 200.0f, 300.0f); light_env.pack_dirty = true;
    light_env.num_dir = 2u;
    for (unsigned i = 0; i < 2u; ++i) {
        light_env.dir_lights[i].direction = jce_v3(.2f + i, -1.0f, .5f);
        light_env.dir_lights[i].color = jce_v3(.4f, .6f, .8f);
        light_env.dir_lights[i].intensity = 2.0f + i;
        light_env.dir_lights[i].cookie_texture = JCE_TEXTURE_INVALID;
        light_env.dir_lights[i].casts_shadow = i == 1u;
    }
    renderer.light_env = &light_env; renderer.renderer = NULL;
    ensure_light_uniforms(); ensure_uniforms(); jce_pbr_material_set_view_mode(9);
    TEST_ASSERT_EQUAL_UINT16(renderer.u_grass_camera.idx, s_lu.camera_pos.idx);
    clear_writes();
}
void tearDown(void) {}

static JceGrassColourReceipt submit_range(uint32_t start, uint32_t count, uint32_t layer)
{
    return sr_grass_submit_range(&renderer, 0, &field, camera, layer,
        (bgfx_program_handle_t){19u}, expected_state, 24u, ROOT_STRIDE,
        (bgfx_vertex_buffer_handle_t){21u}, (bgfx_index_buffer_handle_t){22u},
        16u, 24u, start, count);
}
static void assert_uniform(DrawRecord *draw, const char *name, const float *value, unsigned count)
{
    UniformRecord *record = named(draw->uniform, name);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1u, record->writes, name);
    TEST_ASSERT_EQUAL_FLOAT_ARRAY(value, record->value, count);
}
static void assert_complete_draw(unsigned item, uint32_t start, uint32_t count, int mode)
{
    TEST_ASSERT_LESS_THAN_UINT(draw_count, item); DrawRecord *draw = &draws[item];
    TEST_ASSERT_EQUAL_UINT32(start, draw->start); TEST_ASSERT_EQUAL_UINT32(count, draw->count);
    TEST_ASSERT_EQUAL_UINT64(expected_state, draw->state);
    TEST_ASSERT_EQUAL_UINT16(24u, draw->view); TEST_ASSERT_EQUAL_UINT16(19u, draw->program);
    TEST_ASSERT_EQUAL_UINT16(21u, draw->vertex); TEST_ASSERT_EQUAL_UINT16(22u, draw->index);
    const float pos[4] = {camera ? camera_position.x : 0.0f, camera ? camera_position.y : 0.0f,
        camera ? camera_position.z : 0.0f, 0.0f};
    const float view_mode[4] = {0.0f, 0.0f, (float)mode, 0.0f};
    const float time[4] = {6.75f, 0, 0, 0}, wind[4] = {.6f, .8f, 2.5f, .35f};
    const float fade[4] = {15, 27, .2f, 0};
    const float output[4] = {0, 4, linear_colour ? 1.0f / 2.2f : 1.0f,
        renderer.postfx_tonemap_active ? 1.0f : 0.0f};
    const float colors[8] = {.1f, .2f, .3f, 1, .2f, .4f, .6f, 1};
    assert_uniform(draw, "u_cameraPos", pos, 4u); assert_uniform(draw, "u_normalScale", view_mode, 4u);
    assert_uniform(draw, "u_grass_time", time, 4u); assert_uniform(draw, "u_grass_wind", wind, 4u);
    assert_uniform(draw, "u_grass_fade", fade, 4u); assert_uniform(draw, "u_grass_color", colors, 8u);
    assert_uniform(draw, "u_iblParams", output, 4u);
    assert_uniform(draw, "u_ambientColor", light_env.packed_ambient, 4u);
    assert_uniform(draw, "u_lightCounts", light_env.packed_counts, 4u);
    if (light_env.num_dir > 0u)
        assert_uniform(draw, "u_dirLights", light_env.any_layer_mask ?
            light_env.masked_dir : light_env.packed_dir, light_env.num_dir * 8u);
    assert_uniform(draw, "u_fogParams", renderer.fog_frame.params, 4u);
    if (renderer.fog_frame.params[0] >= .5f) {
        assert_uniform(draw, "u_fogColor", renderer.fog_frame.color, 4u);
        assert_uniform(draw, "u_fogColorSun", renderer.fog_frame.color_sun, 4u);
        assert_uniform(draw, "u_fogHeight", renderer.fog_frame.height, 4u);
    } else {
        TEST_ASSERT_EQUAL_UINT(0u, named(draw->uniform, "u_fogColor")->writes);
        TEST_ASSERT_EQUAL_UINT(0u, named(draw->uniform, "u_fogColorSun")->writes);
        TEST_ASSERT_EQUAL_UINT(0u, named(draw->uniform, "u_fogHeight")->writes);
    }
    TEST_ASSERT_EQUAL_UINT(1u, named(draw->uniform, "u_csmSplits")->writes);
    TEST_ASSERT_EQUAL_FLOAT(renderer.frame_shadow_active ? 12.0f : 0.0f,
        named(draw->uniform, "u_csmSplits")->value[0]);
    assert_uniform(draw, "u_csmParams", shadow_params, 4u);
    assert_uniform(draw, "u_csmBiasScales", shadow_params, 4u);
    const jce_mat4 vp = jce_m4_identity(); assert_uniform(draw, "u_csmVP", (const float *)&vp, 16u);
    for (unsigned i = 0; i < 4u; ++i) {
        char name[32]; snprintf(name, sizeof name, "s_csmShadow%u", i);
        UniformRecord *record = named(draw->uniform, name);
        TEST_ASSERT_EQUAL_UINT(1u, record->writes); TEST_ASSERT_EQUAL_UINT8(9u + i, record->stage);
        TEST_ASSERT_EQUAL_UINT16(60u + i, record->texture); TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, record->flags);
    }
    UniformRecord *cloud = named(draw->uniform, "s_cloudShadow");
    TEST_ASSERT_EQUAL_UINT(1u, cloud->writes); TEST_ASSERT_EQUAL_UINT8(3u, cloud->stage);
    TEST_ASSERT_EQUAL_UINT16(renderer.cloud_shadow_valid ? 71u : 72u, cloud->texture);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, cloud->flags);
    const float cloud_params[4] = {renderer.cloud_shadow_valid ? 84.0f : 0.0f,
        renderer.cloud_shadow_valid ? -6.0f : 0.0f,
        renderer.cloud_shadow_valid ? 9.0f : 0.0f, renderer.cloud_shadow_valid ? 1.0f : 0.0f};
    assert_uniform(draw, "u_cloudShadow", cloud_params, 4u);
    TEST_ASSERT_EQUAL_UINT(0u, named(draw->uniform, "u_baseColorFactor")->writes);
    TEST_ASSERT_EQUAL_UINT(0u, named(draw->uniform, "s_aoMap")->writes);
}
static void assert_no_pending_writes(void)
{
    for (unsigned i = 0; i < uniform_count; ++i)
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, uniforms[i].writes, uniforms[i].name);
}
static void transient_splits_and_distinct_cell_runs_publish_complete_final_state(void)
{
    JceGrassColourReceipt first = submit_range(1u, 7u, 5u);
    JceGrassColourReceipt second = submit_range(12u, 2u, 5u);
    TEST_ASSERT_EQUAL_UINT32(7u, first.accepted); TEST_ASSERT_EQUAL_UINT32(2u, second.accepted);
    TEST_ASSERT_EQUAL_UINT(4u, draw_count);
    assert_complete_draw(0u, 1u, 3u, 9); assert_complete_draw(1u, 4u, 3u, 9);
    assert_complete_draw(2u, 7u, 1u, 9); assert_complete_draw(3u, 12u, 2u, 9);
    TEST_ASSERT_EQUAL_UINT(4u, shadow_calls); TEST_ASSERT_EQUAL_UINT(4u, cloud_calls);
    TEST_ASSERT_EQUAL_UINT(4u, fog_calls); assert_no_pending_writes();
}
static void resident_cell_runs_keep_root_ranges_and_each_draw_state(void)
{
    persist = true;
    TEST_ASSERT_EQUAL_UINT32(4u, submit_range(2u, 4u, 5u).accepted);
    TEST_ASSERT_EQUAL_UINT32(2u, submit_range(11u, 2u, 5u).accepted);
    TEST_ASSERT_EQUAL_UINT(0u, allocation_calls); TEST_ASSERT_EQUAL_UINT(0u, availability_calls);
    TEST_ASSERT_EQUAL_UINT(2u, roots_calls); TEST_ASSERT_EQUAL_UINT(2u, draw_count);
    assert_complete_draw(0u, 2u, 4u, 9); assert_complete_draw(1u, 11u, 2u, 9);
}
static void capacity_and_allocation_rejection_publish_no_unsubmitted_uniforms(void)
{
    capacity = 0u; JceGrassColourReceipt result = submit_range(0u, 5u, 5u);
    TEST_ASSERT_EQUAL_UINT32(0u, result.accepted); TEST_ASSERT_EQUAL_UINT(1u, result.reason);
    TEST_ASSERT_EQUAL_UINT(0u, draw_count); assert_no_pending_writes();
    capacity = ROOT_CAP; short_allocation = 1u; result = submit_range(0u, 5u, 5u);
    TEST_ASSERT_EQUAL_UINT32(0u, result.accepted); TEST_ASSERT_EQUAL_UINT(2u, result.reason);
    TEST_ASSERT_EQUAL_UINT(0u, shadow_calls); assert_no_pending_writes();
    short_allocation = 0u; empty_allocation = 3u; result = submit_range(4u, 7u, 5u);
    TEST_ASSERT_EQUAL_UINT32(3u, result.accepted); TEST_ASSERT_EQUAL_UINT32(4u, result.rejected);
    TEST_ASSERT_EQUAL_UINT(1u, draw_count); assert_complete_draw(0u, 4u, 3u, 9);
    assert_no_pending_writes();
}
static void unavailable_resident_roots_keep_the_complete_transient_split_path(void)
{
    persist = true; roots_ready = false;
    TEST_ASSERT_EQUAL_UINT32(5u, submit_range(4u, 5u, 5u).accepted);
    TEST_ASSERT_EQUAL_UINT(1u, roots_calls); TEST_ASSERT_EQUAL_UINT(2u, allocation_calls);
    assert_complete_draw(0u, 4u, 3u, 9); assert_complete_draw(1u, 7u, 2u, 9);
}
static void missing_fallback_instances_reject_before_binding(void)
{
    renderer.grass_cache[0].packed = NULL; renderer.grass_cache[0].insts = NULL;
    JceGrassColourReceipt result = submit_range(0u, 2u, 5u);
    TEST_ASSERT_EQUAL_UINT32(0u, result.accepted); TEST_ASSERT_EQUAL_UINT(2u, result.reason);
    TEST_ASSERT_EQUAL_UINT(0u, draw_count); assert_no_pending_writes();
}
static void compare_directional_with_full_light_upload(uint32_t layer)
{
    jce_light_env_apply_for_layer(&light_env, NULL, layer);
    UniformRecord ambient = *named(uniforms, "u_ambientColor");
    UniformRecord counts = *named(uniforms, "u_lightCounts");
    UniformRecord directional = *named(uniforms, "u_dirLights");
    clear_writes(); TEST_ASSERT_EQUAL_UINT32(2u, submit_range(0u, 2u, layer).accepted);
    assert_complete_draw(0u, 0u, 2u, 9);
    UniformRecord *actual = draws[0].uniform;
    assert_uniform(&draws[0], "u_ambientColor", ambient.value, 4u);
    assert_uniform(&draws[0], "u_lightCounts", counts.value, 4u);
    TEST_ASSERT_EQUAL_UINT(directional.writes, named(actual, "u_dirLights")->writes);
    if (directional.writes)
        assert_uniform(&draws[0], "u_dirLights", directional.value, (unsigned)directional.count * 4u);
    TEST_ASSERT_EQUAL_FLOAT(100.0f, light_env.camera_pos.x);
}
static void directional_subset_matches_full_light_upload_and_preserves_masked_slots(void)
{
    light_env.dir_lights[1].layer_mask = 1u << 3u;
    compare_directional_with_full_light_upload(5u);
    TEST_ASSERT_EQUAL_FLOAT(2.0f, named(draws[0].uniform, "u_lightCounts")->value[3]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, named(draws[0].uniform, "u_dirLights")->value[11]);
    TEST_ASSERT_EQUAL_FLOAT(2.0f, named(draws[0].uniform, "u_dirLights")->value[3]);
}
static void zero_directional_lights_publish_current_zero_counts_and_ambient(void)
{
    light_env.num_dir = 0u; compare_directional_with_full_light_upload(31u);
    const float zero[4] = {0, 0, 0, 0}; assert_uniform(&draws[0], "u_lightCounts", zero, 4u);
}
static void final_camera_view_modes_and_disabled_frame_inputs_keep_exact_values(void)
{
    const int modes[] = {0, 2, 8, 10};
    renderer.frame_shadow_active = false; renderer.cloud_shadow_valid = false;
    renderer.fog_frame.params[0] = 0.0f; renderer.postfx_tonemap_active = false;
    linear_colour = false; camera = NULL; persist = true;
    for (unsigned i = 0; i < sizeof modes / sizeof modes[0]; ++i) {
        jce_pbr_material_set_view_mode(modes[i]);
        TEST_ASSERT_EQUAL_UINT32(1u, submit_range(i, 1u, 5u).accepted);
        assert_complete_draw(i, i, 1u, modes[i]);
    }
}
static void shadow_geometry_route_keeps_camera_and_deformation_without_colour_inputs(void)
{
    sr_grass_bind_geometry_uniforms(&renderer, &field, camera);
    const float pos[4] = {4, 8, -12, 0};
    TEST_ASSERT_EQUAL_UINT(1u, named(uniforms, "u_cameraPos")->writes);
    TEST_ASSERT_EQUAL_FLOAT_ARRAY(pos, named(uniforms, "u_cameraPos")->value, 4u);
    TEST_ASSERT_EQUAL_UINT(1u, named(uniforms, "u_grass_time")->writes);
    TEST_ASSERT_EQUAL_UINT(0u, named(uniforms, "u_grass_color")->writes);
    TEST_ASSERT_EQUAL_UINT(0u, named(uniforms, "u_normalScale")->writes);
    TEST_ASSERT_EQUAL_UINT(0u, shadow_calls); TEST_ASSERT_EQUAL_UINT(0u, draw_count);
}
static void expanded_range_uses_no_instance_buffer(void)
{
    expanded_grass = true;
    capacity = 0u;
    persist = true;
    const JceGrassColourReceipt receipt = submit_range(2u, 2u, 0u);
    TEST_ASSERT_EQUAL_UINT32(2u, receipt.accepted);
    TEST_ASSERT_EQUAL_UINT(1u, draw_count);
    TEST_ASSERT_EQUAL_UINT32(2u, draws[0].start);
    TEST_ASSERT_EQUAL_UINT32(2u, draws[0].count);
    TEST_ASSERT_EQUAL_UINT(0u, allocation_calls + availability_calls + roots_calls);
    assert_no_pending_writes();
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(transient_splits_and_distinct_cell_runs_publish_complete_final_state);
    RUN_TEST(resident_cell_runs_keep_root_ranges_and_each_draw_state);
    RUN_TEST(expanded_range_uses_no_instance_buffer);
    RUN_TEST(capacity_and_allocation_rejection_publish_no_unsubmitted_uniforms);
    RUN_TEST(unavailable_resident_roots_keep_the_complete_transient_split_path);
    RUN_TEST(missing_fallback_instances_reject_before_binding);
    RUN_TEST(directional_subset_matches_full_light_upload_and_preserves_masked_slots);
    RUN_TEST(zero_directional_lights_publish_current_zero_counts_and_ambient);
    RUN_TEST(final_camera_view_modes_and_disabled_frame_inputs_keep_exact_values);
    RUN_TEST(shadow_geometry_route_keeps_camera_and_deformation_without_colour_inputs);
    return UNITY_END();
}
