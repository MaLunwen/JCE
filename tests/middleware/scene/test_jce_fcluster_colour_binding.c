/* Exact cluster emission, camera lifetime and canonical shadow-frame owners.
 * Typed CPU ports count aliased uniforms/samplers and erase state on ALL submit. */
#include "unity.h"
#include "middleware/scene/jce_sr_fcluster_colour.h"
#include "middleware/scene/jce_sr_shadow_frame.h"
#include "renderer/jce_lighting_system_internal.h"
#include "renderer/jce_render_encoder.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct Uniform {
    char name[40]; unsigned writes; bgfx_uniform_type_t type;
    float value[16]; uint8_t stage; uint16_t texture;
} Uniform;
static Uniform uniforms[64], draws[8][64];
static unsigned names, submits, acquires, textures_created, allocations, resources;
static unsigned instance_queries;
static uint32_t available_count, allocated_count;
static bool null_allocation, shadows_enabled, expanded;
static int mesh_token;
static const JceMesh *expected_mesh;
static uint16_t vertex_handle, index_handle;
static unsigned char data[4 * 32];
static float instances[4 * 8];
static JceSceneRenderer renderer;
static JceFoliageClusterComponent field;
static jce_vec3 position;
static const JceCamera *camera;
static int mode, cvar_token;
static uint32_t instance_count;
static uint64_t state;
static const uint64_t expected_state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_Z |
    BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_MSAA;

static bgfx_uniform_handle_t acquire(const char *name, bgfx_uniform_type_t type, uint16_t count)
{
    (void)count; ++acquires;
    for (unsigned i = 0; i < names; ++i)
        if (strcmp(uniforms[i].name, name) == 0) return (bgfx_uniform_handle_t){(uint16_t)i};
    TEST_ASSERT_LESS_THAN_UINT(64u, names);
    unsigned item = names++; snprintf(uniforms[item].name, 40u, "%s", name);
    uniforms[item].type = type;
    return (bgfx_uniform_handle_t){(uint16_t)item};
}
static Uniform *named(Uniform *records, const char *name)
{
    for (unsigned i = 0; i < names; ++i)
        if (strcmp(records[i].name, name) == 0) return &records[i];
    TEST_FAIL_MESSAGE("Required named uniform was not acquired"); return NULL;
}
static void set_uniform(bgfx_uniform_handle_t handle, const void *value, uint16_t count)
{
    TEST_ASSERT_LESS_THAN_UINT(names, handle.idx); Uniform *u = &uniforms[handle.idx];
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, u->writes, u->name);
    TEST_ASSERT_LESS_OR_EQUAL_UINT(4u, count); ++u->writes;
    memcpy(u->value, value, (size_t)count * 4u * sizeof(float));
}
static void set_texture(uint8_t stage, bgfx_uniform_handle_t sampler,
    bgfx_texture_handle_t texture, uint32_t flags)
{
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, flags); Uniform *u = &uniforms[sampler.idx];
    TEST_ASSERT_EQUAL_INT(BGFX_UNIFORM_TYPE_SAMPLER, u->type);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, u->writes, u->name);
    ++u->writes; u->stage = stage; u->texture = texture.idx;
}
static void clear_writes(void)
{ for (unsigned i = 0; i < names; ++i) { uniforms[i].writes = 0u; memset(uniforms[i].value, 0xcd, sizeof uniforms[i].value); } }
static void no_pending_writes(void)
{ for (unsigned i = 0; i < names; ++i) TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, uniforms[i].writes, uniforms[i].name); }
static const bgfx_memory_t *copy_memory(const void *bytes, uint32_t size)
{
    static uint8_t storage[8]; static bgfx_memory_t memory;
    TEST_ASSERT_EQUAL_UINT32(textures_created == 0u ? 4u : 8u, size);
    memcpy(storage, bytes, size);
    memory.data = storage; memory.size = size; return &memory;
}
static const bgfx_memory_t *alloc_memory(uint32_t size)
{ (void)size; TEST_FAIL_MESSAGE("Unexpected cookie atlas allocation"); return NULL; }
static const bgfx_caps_t *caps_port(void)
{
    /* Allow the modern white array placeholder without modelling a blit atlas. */
    static const bgfx_caps_t arrays = {
        .supported = BGFX_CAPS_TEXTURE_2D_ARRAY,
        .limits = {.maxTextureLayers = 2u},
    };
    static const bgfx_caps_t no_arrays = {0};
    return expanded ? &no_arrays : &arrays;
}
static bgfx_texture_handle_t create_texture(uint16_t w, uint16_t h, bool mips,
    uint16_t layers, bgfx_texture_format_t format, uint64_t flags,
    const bgfx_memory_t *memory, uint64_t external)
{
    TEST_ASSERT_FALSE(mips);
    TEST_ASSERT_EQUAL_INT(BGFX_TEXTURE_FORMAT_RGBA8, format);
    TEST_ASSERT_EQUAL_UINT64(BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, flags);
    TEST_ASSERT_EQUAL_UINT64(0u, external);
    TEST_ASSERT_EQUAL_UINT16(1u, w); TEST_ASSERT_EQUAL_UINT16(1u, h);
    TEST_ASSERT_LESS_THAN_UINT(2u, textures_created);
    TEST_ASSERT_EQUAL_UINT16(textures_created == 0u ? 1u : 2u, layers);
    TEST_ASSERT_NOT_NULL(memory);
    TEST_ASSERT_EQUAL_UINT32(4u * layers, memory->size);
    for (uint32_t i = 0; i < memory->size; ++i)
        TEST_ASSERT_EQUAL_HEX8(0xff, memory->data[i]);
    return (bgfx_texture_handle_t){(uint16_t)(50u + textures_created++)};
}
static void log_port(JceLogLevel level, const char *tag, const char *file, int line, const char *format, ...)
{
    (void)level; (void)file; (void)line; va_list args; va_start(args, format);
    fprintf(stderr, "%s: ", tag); vfprintf(stderr, format, args); fputc('\n', stderr); va_end(args);
}
static JceCvar *register_bool(const char *name, bool def, uint32_t flags, const char *help)
{
    TEST_ASSERT_EQUAL_STRING("r.shadows", name); TEST_ASSERT_TRUE(def);
    TEST_ASSERT_EQUAL_UINT32(JCE_CVAR_FLAG_NONE, flags); TEST_ASSERT_NOT_NULL(help);
    return (JceCvar *)&cvar_token;
}
static bool get_bool(const JceCvar *value)
{ TEST_ASSERT_EQUAL_PTR(&cvar_token, value); return shadows_enabled; }
static uint32_t instance_available(uint32_t count, uint16_t stride)
{ (void)count; ++instance_queries; TEST_ASSERT_EQUAL_UINT16(32u, stride); return available_count; }
static void instance_allocate(bgfx_instance_data_buffer_t *buffer, uint32_t count, uint16_t stride)
{
    (void)count; ++allocations; memset(buffer, 0, sizeof *buffer);
    buffer->data = null_allocation ? NULL : data; buffer->num = allocated_count; buffer->stride = stride;
}
static uint32_t get_vertex(const JceMesh *mesh)
{ TEST_ASSERT_EQUAL_PTR(expected_mesh, mesh); return vertex_handle; }
static uint32_t get_index(const JceMesh *mesh)
{ TEST_ASSERT_EQUAL_PTR(expected_mesh, mesh); return index_handle; }
static uint32_t set_transform(const void *matrix, uint16_t count)
{ const jce_mat4 identity = jce_m4_identity(); TEST_ASSERT_EQUAL_UINT16(1u, count); TEST_ASSERT_EQUAL_MEMORY(&identity, matrix, sizeof identity); return 0u; }
static void set_vertex(uint8_t stream, bgfx_vertex_buffer_handle_t handle, uint32_t start, uint32_t count)
{
    TEST_ASSERT_EQUAL_UINT8(0u, stream); TEST_ASSERT_EQUAL_UINT16(21u, handle.idx);
    TEST_ASSERT_EQUAL_UINT32(0u, start); TEST_ASSERT_EQUAL_UINT32(expanded ? 12u : 4u, count);
}
static void set_index(bgfx_index_buffer_handle_t handle, uint32_t start, uint32_t count)
{
    TEST_ASSERT_EQUAL_UINT16(22u, handle.idx); TEST_ASSERT_EQUAL_UINT32(0u, start);
    TEST_ASSERT_EQUAL_UINT32(expanded ? 18u : 6u, count);
}
static void set_instances(const bgfx_instance_data_buffer_t *buffer, uint32_t start, uint32_t count)
{
    TEST_ASSERT_FALSE(expanded); TEST_ASSERT_EQUAL_UINT32(0u, start);
    TEST_ASSERT_EQUAL_MEMORY(instances, buffer->data, count * 32u); instance_count = count;
}
static void set_state(uint64_t value, uint32_t rgba)
{ TEST_ASSERT_EQUAL_UINT32(0u, rgba); state = value; }
static void submit_port(bgfx_view_id_t view, bgfx_program_handle_t program, uint32_t depth, uint8_t discard)
{
    TEST_ASSERT_EQUAL_UINT16(24u, view); TEST_ASSERT_EQUAL_UINT16(19u, program.idx);
    TEST_ASSERT_EQUAL_UINT32(0u, depth); TEST_ASSERT_EQUAL_UINT8(BGFX_DISCARD_ALL, discard);
    TEST_ASSERT_EQUAL_UINT64(expected_state, state);
    TEST_ASSERT_EQUAL_UINT32(expanded ? 0u : 3u, instance_count);
    TEST_ASSERT_LESS_THAN_UINT(8u, submits); memcpy(draws[submits++], uniforms, sizeof uniforms);
    clear_writes(); state = 0u; instance_count = 0u;
}
static void view_mode_port(void)
{ const float value[4] = {0, 0, (float)mode, 0}; set_uniform(acquire("u_normalScale", BGFX_UNIFORM_TYPE_VEC4, 1u), value, 1u); }
static jce_vec3 camera_port(const JceCamera *value)
{ TEST_ASSERT_EQUAL_PTR(camera, value); return position; }
static uint32_t frame_port(const JceRenderer *value)
{ (void)value; TEST_FAIL_MESSAGE("Full light packing is outside the camera-only fixture"); return 0u; }

static bgfx_renderer_type_t backend_port(void)
{
    return BGFX_RENDERER_TYPE_OPENGL;
}

static JceRendererApiInfo api_info_port(void)
{
    return (JceRendererApiInfo){
        .backend = JCE_BACKEND_OPENGL,
        .build_tier = expanded
            ? JCE_GRAPHICS_API_TIER_COMPATIBILITY : JCE_GRAPHICS_API_TIER_STABLE,
    };
}

#define jce_enc_set_uniform set_uniform
#define jce_enc_set_texture set_texture
#define bgfx_create_uniform acquire
#define bgfx_copy copy_memory
#define bgfx_alloc alloc_memory
#define bgfx_get_caps caps_port
#define bgfx_create_texture_2d create_texture
#define jce_log_write log_port
#define jce_renderer_get_frame_index frame_port
#undef LOG_TAG
#define bgfx_get_renderer_type backend_port
#define jce_renderer_get_api_info api_info_port
#include "renderer/jce_lighting_system.c"
#undef jce_renderer_get_api_info
#undef bgfx_get_renderer_type
#undef LOG_TAG
#define jce_cvar_register_bool register_bool
#define jce_cvar_get_bool get_bool
#include "middleware/scene/jce_sr_shadow_frame.c"
#define bgfx_set_uniform set_uniform
#define bgfx_set_texture set_texture
#define bgfx_get_avail_instance_data_buffer instance_available
#define bgfx_alloc_instance_data_buffer instance_allocate
#define jce_mesh_get_vbh get_vertex
#define jce_mesh_get_ibh get_index
#define bgfx_set_transform set_transform
#define bgfx_set_vertex_buffer set_vertex
#define bgfx_set_index_buffer set_index
#define bgfx_set_instance_data_buffer set_instances
#define bgfx_set_state set_state
#define bgfx_submit submit_port
#define jce_pbr_material_bind_view_mode view_mode_port
#define jce_camera_get_position camera_port
#ifndef JCE_FCLUSTER_COLOUR_OWNER
#define JCE_FCLUSTER_COLOUR_OWNER "middleware/scene/jce_sr_fcluster_colour.c"
#endif
#include JCE_FCLUSTER_COLOUR_OWNER

bool sr_resolve_primary_dir_light(JceSceneRenderer *sr, JceScene *scene, EntityList *list,
    bool shadow_only, jce_vec3 *to_light, jce_vec3 *color, float *intensity)
{
    (void)scene; (void)list; (void)color; (void)intensity; TEST_ASSERT_EQUAL_PTR(&renderer, sr);
    TEST_ASSERT_FALSE(shadow_only); *to_light = jce_v3(.2f, .9f, .3f); ++resources; return true;
}
JceTexture sr_resolve_texture(JceSceneRenderer *sr, const char *path)
{ TEST_ASSERT_EQUAL_PTR(&renderer, sr); TEST_ASSERT_EQUAL_STRING("leaf.png", path); ++resources; return (JceTexture){77u}; }
void sr_apply_fog_uniforms(JceSceneRenderer *sr)
{
    set_uniform(sr->u_fog_params, sr->fog_frame.params, 1u);
    if (sr->fog_frame.params[0] >= .5f) {
        set_uniform(sr->u_fog_color, sr->fog_frame.color, 1u);
        set_uniform(sr->u_fog_color_sun, sr->fog_frame.color_sun, 1u);
        set_uniform(sr->u_fog_height, sr->fog_frame.height, 1u);
    }
}
void sr_bind_cloud_shadow(JceSceneRenderer *sr)
{
    const float cloud[4] = {0, 0, 0, 0};
    set_texture(3u, sr->s_cloud_shadow, sr->white_tex, UINT32_MAX);
    set_uniform(sr->u_cloud_shadow, cloud, 1u);
}
void setUp(void)
{
    memset(&renderer, 0, sizeof renderer); memset(&field, 0, sizeof field);
    memset(uniforms, 0, sizeof uniforms); memset(draws, 0, sizeof draws); memset(&s_lu, 0, sizeof s_lu);
    names = submits = acquires = textures_created = allocations = resources = 0u;
    instance_queries = 0u; expanded = false;
    available_count = allocated_count = 4u; null_allocation = false; shadows_enabled = true;
    vertex_handle = 21u; index_handle = 22u; mode = 9; state = 0u; instance_count = 0u;
    position = jce_v3(4, 8, -12); camera = (const JceCamera *)&position;
    renderer.fcluster_quad = (JceMesh *)&renderer; renderer.prog_fcluster.idx = 19u;
    expected_mesh = renderer.fcluster_quad;
    renderer.white_tex.idx = 72u; renderer.water_time = 6.75f;
#define HANDLE(member, name, type, count) renderer.member = acquire(name, type, count)
    HANDLE(u_foliage_time, "u_foliage_time", BGFX_UNIFORM_TYPE_VEC4, 1u);
    HANDLE(u_foliage_colors, "u_foliage_colors", BGFX_UNIFORM_TYPE_VEC4, 4u);
    HANDLE(u_foliage_light, "u_foliage_light", BGFX_UNIFORM_TYPE_VEC4, 1u);
    HANDLE(s_foliage_alpha, "s_foliageAlpha", BGFX_UNIFORM_TYPE_SAMPLER, 1u);
    HANDLE(u_csm_splits, "u_csmSplits", BGFX_UNIFORM_TYPE_VEC4, 1u);
    HANDLE(u_fog_params, "u_fogParams", BGFX_UNIFORM_TYPE_VEC4, 1u);
    HANDLE(u_fog_color, "u_fogColor", BGFX_UNIFORM_TYPE_VEC4, 1u);
    HANDLE(u_fog_color_sun, "u_fogColorSun", BGFX_UNIFORM_TYPE_VEC4, 1u);
    HANDLE(u_fog_height, "u_fogHeight", BGFX_UNIFORM_TYPE_VEC4, 1u);
    HANDLE(s_cloud_shadow, "s_cloudShadow", BGFX_UNIFORM_TYPE_SAMPLER, 1u);
    HANDLE(u_cloud_shadow, "u_cloudShadow", BGFX_UNIFORM_TYPE_VEC4, 1u);
#undef HANDLE
    renderer.frame_shadow_active = renderer.shadow_use_csm = renderer.last_csm_valid = true;
    renderer.last_csm.cascade_count = 2u; renderer.last_csm.splits[0] = .1f;
    renderer.last_csm.splits[1] = 12.0f; renderer.last_csm.splits[2] = 80.0f;
    renderer.fog_frame.params[0] = 2.0f; renderer.fog_frame.color[1] = .7f;
    renderer.fog_frame.color_sun[2] = .8f; renderer.fog_frame.height[3] = 4.0f;
    for (unsigned i = 0; i < 3u; ++i) {
        field.shadow_color[i] = .1f * (i + 1u); field.mid_color[i] = .2f * (i + 1u);
        field.highlight_color[i] = .3f * (i + 1u); field.color_multiplier[i] = .4f * (i + 1u);
    }
    for (unsigned i = 0; i < 32u; ++i) instances[i] = (float)i;
    snprintf(field.alpha_tex, sizeof field.alpha_tex, "%s", "leaf.png");
}
void tearDown(void) {}
static JceGrassColourReceipt emit(void)
{ return sr_fcluster_submit_colour(&renderer, NULL, NULL, &field, camera, 24u, instances, 3u); }
static void expect(unsigned draw, const char *name, const float *value, unsigned count)
{ Uniform *u = named(draws[draw], name); TEST_ASSERT_EQUAL_UINT_MESSAGE(1u, u->writes, name); TEST_ASSERT_EQUAL_FLOAT_ARRAY(value, u->value, count); }
static void expect_draw(unsigned draw, bool active_splits)
{
    const float cam[4] = {camera ? position.x : 0, camera ? position.y : 0, camera ? position.z : 0, 0};
    const float splits[4] = {active_splits ? 12.0f : 0, active_splits ? 80.0f : 0, active_splits ? 80.0f : 0, active_splits ? 80.0f : 0};
    const float view[4] = {0, 0, (float)mode, 0}, light[4] = {.2f, .9f, .3f, 0}, time[4] = {6.75f, 0, 0, 0};
    const float colors[16] = {.1f,.2f,.3f,0, .2f,.4f,.6f,0, .3f,.6f,.9f,0, .4f,.8f,1.2f,1};
    expect(draw, "u_cameraPos", cam, 4u); expect(draw, "u_csmSplits", splits, 4u);
    expect(draw, "u_normalScale", view, 4u); expect(draw, "u_foliage_light", light, 4u);
    expect(draw, "u_foliage_time", time, 4u); expect(draw, "u_foliage_colors", colors, 16u);
    expect(draw, "u_fogParams", renderer.fog_frame.params, 4u);
    if (renderer.fog_frame.params[0] >= .5f) {
        expect(draw, "u_fogColor", renderer.fog_frame.color, 4u);
        expect(draw, "u_fogColorSun", renderer.fog_frame.color_sun, 4u);
        expect(draw, "u_fogHeight", renderer.fog_frame.height, 4u);
    } else TEST_ASSERT_EQUAL_UINT(0u, named(draws[draw], "u_fogColor")->writes);
    Uniform *alpha = named(draws[draw], "s_foliageAlpha"), *cloud = named(draws[draw], "s_cloudShadow");
    TEST_ASSERT_EQUAL_UINT(1u, alpha->writes); TEST_ASSERT_EQUAL_UINT8(0u, alpha->stage);
    TEST_ASSERT_EQUAL_UINT16(field.alpha_tex[0] ? 77u : 72u, alpha->texture);
    TEST_ASSERT_EQUAL_UINT(1u, cloud->writes); TEST_ASSERT_EQUAL_UINT8(3u, cloud->stage);
    TEST_ASSERT_EQUAL_UINT16(72u, cloud->texture);
}
static void repeated_clusters_publish_complete_state_and_reuse_camera_lifetime(void)
{
    TEST_ASSERT_EQUAL_UINT32(3u, emit().accepted); expect_draw(0u, true);
    unsigned acquired = acquires, created = textures_created;
    TEST_ASSERT_TRUE(s_lu.init); TEST_ASSERT_EQUAL_UINT(2u, created);
    TEST_ASSERT_EQUAL_UINT16(50u, s_lu.white_1x1.idx);
    TEST_ASSERT_EQUAL_UINT16(51u, s_lu.cookie_texture.texture.idx);
    TEST_ASSERT_FALSE(s_lu.cookie_texture.atlas_enabled);
    TEST_ASSERT_TRUE(BGFX_HANDLE_IS_VALID(s_lu.s_cookie));
    position = jce_v3(-3, 6, 10); mode = 8;
    TEST_ASSERT_EQUAL_UINT32(3u, emit().accepted); expect_draw(1u, true);
    TEST_ASSERT_EQUAL_UINT(acquired + 1u, acquires); /* Existing view-mode test port acquires its alias. */
    TEST_ASSERT_EQUAL_UINT(created, textures_created); TEST_ASSERT_EQUAL_UINT(2u, submits); no_pending_writes();
}
static void geometry_rejection_leaves_no_pending_draw_and_next_cluster_is_complete(void)
{
    vertex_handle = UINT16_MAX; JceGrassColourReceipt r = emit();
    TEST_ASSERT_EQUAL_UINT(JCE_FCLUSTER_GEOMETRY_UNAVAILABLE, r.reason);
    TEST_ASSERT_EQUAL_UINT32(0u, r.accepted); TEST_ASSERT_EQUAL_UINT(0u, resources); no_pending_writes();
    vertex_handle = 21u; TEST_ASSERT_EQUAL_UINT32(3u, emit().accepted); expect_draw(0u, true);
}
static void allocation_rejections_never_publish_frame_or_custom_uniforms(void)
{
    available_count = 2u; TEST_ASSERT_EQUAL_UINT(JCE_FCLUSTER_CAPACITY, emit().reason);
    available_count = 4u; allocated_count = 2u;
    TEST_ASSERT_EQUAL_UINT(JCE_FCLUSTER_ALLOCATION_SHORT, emit().reason);
    allocated_count = 4u; null_allocation = true;
    TEST_ASSERT_EQUAL_UINT(JCE_FCLUSTER_ALLOCATION_NULL, emit().reason);
    TEST_ASSERT_EQUAL_UINT(0u, submits); TEST_ASSERT_EQUAL_UINT(0u, resources); no_pending_writes();
}
static void expanded_cards_submit_full_geometry_without_instance_capacity(void)
{
    const JceMesh *mesh = (const JceMesh *)&mesh_token;
    expanded = true;
    expected_mesh = mesh;
    available_count = 0u;
    null_allocation = true;

    index_handle = UINT16_MAX;
    JceGrassColourReceipt receipt = sr_fcluster_submit_colour_mesh(
        &renderer, NULL, NULL, &field, camera, 24u, mesh, 3u);
    TEST_ASSERT_EQUAL_UINT(JCE_FCLUSTER_GEOMETRY_UNAVAILABLE, receipt.reason);
    TEST_ASSERT_EQUAL_UINT32(0u, receipt.accepted);
    TEST_ASSERT_EQUAL_UINT32(3u, receipt.rejected);
    TEST_ASSERT_EQUAL_UINT(0u, resources);
    TEST_ASSERT_EQUAL_UINT(0u, submits);
    no_pending_writes();

    index_handle = 22u;
    receipt = sr_fcluster_submit_colour_mesh(
        &renderer, NULL, NULL, &field, camera, 24u, mesh, 3u);
    TEST_ASSERT_EQUAL_UINT(JCE_FCLUSTER_COMPLETE, receipt.reason);
    TEST_ASSERT_EQUAL_UINT32(3u, receipt.requested);
    TEST_ASSERT_EQUAL_UINT32(3u, receipt.accepted);
    TEST_ASSERT_EQUAL_UINT32(0u, receipt.rejected);
    TEST_ASSERT_EQUAL_UINT(1u, submits);
    TEST_ASSERT_EQUAL_UINT(0u, instance_queries);
    TEST_ASSERT_EQUAL_UINT(0u, allocations);
    TEST_ASSERT_EQUAL_UINT(1u, textures_created);
    TEST_ASSERT_EQUAL_INT(JCE_LIGHT_COOKIE_COMPATIBILITY, s_lu.cookie_texture.reason);
    TEST_ASSERT_FALSE(BGFX_HANDLE_IS_VALID(s_lu.cookie_texture.texture));
    TEST_ASSERT_FALSE(BGFX_HANDLE_IS_VALID(s_lu.s_cookie));
    expect_draw(0u, true);
    no_pending_writes();
}
static void disabled_shadow_modes_publish_zero_splits_without_shadow_samplers(void)
{
    shadows_enabled = false; TEST_ASSERT_EQUAL_UINT32(3u, emit().accepted); expect_draw(0u, false);
    shadows_enabled = true; renderer.frame_shadow_active = false;
    TEST_ASSERT_EQUAL_UINT32(3u, emit().accepted); expect_draw(1u, false);
    renderer.frame_shadow_active = true; renderer.shadow_use_csm = false;
    TEST_ASSERT_EQUAL_UINT32(3u, emit().accepted); expect_draw(2u, false);
    renderer.shadow_use_csm = true; renderer.last_csm_valid = false;
    TEST_ASSERT_EQUAL_UINT32(3u, emit().accepted); expect_draw(3u, false);
    for (unsigned i = 0; i < names; ++i) TEST_ASSERT_FALSE(strncmp(uniforms[i].name, "s_csm", 5u) == 0);
}
static void absent_camera_fog_and_alpha_keep_neutral_final_values(void)
{
    camera = NULL; field.alpha_tex[0] = '\0'; renderer.fog_frame.params[0] = 0;
    mode = 10; TEST_ASSERT_EQUAL_UINT32(3u, emit().accepted); expect_draw(0u, true);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(repeated_clusters_publish_complete_state_and_reuse_camera_lifetime);
    RUN_TEST(geometry_rejection_leaves_no_pending_draw_and_next_cluster_is_complete);
    RUN_TEST(allocation_rejections_never_publish_frame_or_custom_uniforms);
    RUN_TEST(expanded_cards_submit_full_geometry_without_instance_capacity);
    RUN_TEST(disabled_shadow_modes_publish_zero_splits_without_shadow_samplers);
    RUN_TEST(absent_camera_fog_and_alpha_keep_neutral_final_values);
    return UNITY_END();
}
