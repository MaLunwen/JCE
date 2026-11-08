/* Complete actual model producer and encoder owners, with typed CPU ports.
 * No device, shader execution, physical sampler acceptance or pixels are tested.
 */
#include "unity.h"
#include <bgfx/c99/bgfx.h>
#include <jce/renderer/jce_mesh.h>
#include <jce/renderer/jce_skinned_mesh.h>
#include <jce/renderer/jce_skin_palette.h>
#include <jce/renderer/jce_texture.h>
#include "renderer/jce_model_deform_internal.h"
#include "renderer/jce_model_internal.h"
#include "renderer/jce_model_toon_hair_internal.h"
#include "renderer/jce_skinned_mesh_deform_internal.h"
#include "renderer/jce_model_instancing.h"
#include "renderer/jce_renderer_internal.h"
#include "renderer/jce_gltf_loader.h"
#include "renderer/jce_instance_upload.h"
#include "renderer/jce_shader_deferred.h"
#include "os/core/jce_memory.h"
#include <string.h>

typedef struct ModelSubmit {
    uint16_t view, program, indirect;
    uint32_t start, num, depth, instance_start, instance_count;
    uint8_t flags;
    unsigned callback, geometry, late_binds;
    uint32_t sampler_flags[4];
    uint16_t samplers[4], textures[4];
    unsigned sampler_uniform_count;
    uint16_t sampler_uniforms[8];
} ModelSubmit;
static ModelSubmit submits[8];
static unsigned submit_count, callback_count, material_count, geometry_count;
static unsigned late_bind_count, explicit_bind_count, explicit_submit_count;
static unsigned resolve_count, transient_count, instance_bind_count;
static uint16_t static_program, instance_program, tint_program, source_buffer;
static uint32_t source_start, source_count;
static uint32_t bound_flags[4];
static uint16_t bound_samplers[4], bound_textures[4];
/* Sampler uniforms share bgfx's single-set check even when a stage is rebound.
 * Dependency resets below affect final stage values, never this pending set. */
static uint16_t pending_sampler_uniforms[8];
static unsigned pending_sampler_uniform_count;
static uint8_t upload_data[4 * 80], captured_payload[4 * 80];
static uint16_t payload_stride;
static uint32_t payload_count;
static JceInstanceUploadOwner upload_owner;
static JcePbrMaterial last_material;
static JceModel model;
static JceModelNode node;
static JceModelPrimitive primitives[2];
static JcePbrMaterial material;
static unsigned char renderer_token, mesh_tokens[2], encoder_token;
static const JceRenderer *renderer;
static const uint32_t linear_compare = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP |
    BGFX_SAMPLER_MIP_POINT | BGFX_SAMPLER_COMPARE_LEQUAL;
/* Each real producer calls material/geometry/callback dependencies before its
 * terminal submit. Deliberately erase sampler state at every dependency edge:
 * preparing too early cannot satisfy the final-state assertions. The sampler
 * uniform debug set survives all of these dependency boundaries. */
static void reset_sampler_state(void)
{
    for (unsigned i = 0; i < 4; ++i) {
        bound_flags[i] = linear_compare;
        bound_samplers[i] = (uint16_t)(90 + i);
        bound_textures[i] = (uint16_t)(80 + i);
    }
    late_bind_count = 0;
}
static void counted_material(const JcePbrMaterial *mat, const JceRenderer *r, uint16_t view)
{
    (void)r; (void)view;
    last_material = *mat;
    ++material_count;
    reset_sampler_state();
}
static void counted_geometry(const JceSkinnedMesh *mesh, const JceRenderer *r, uint16_t view)
{
    (void)mesh; (void)r; (void)view;
    ++geometry_count;
    reset_sampler_state();
}
static void counted_geometry_lod(const JceSkinnedMesh *mesh, const JceRenderer *r,
                                  uint16_t view, uint32_t level)
{ (void)level; counted_geometry(mesh, r, view); }
static void record_texture(uint8_t stage, bgfx_uniform_handle_t sampler,
    bgfx_texture_handle_t texture, uint32_t flags);
static void counted_callback(void *user, uint16_t view)
{
    TEST_ASSERT_EQUAL_PTR(&model, user);
    TEST_ASSERT_TRUE(view == 23 || view == 24);
    ++callback_count;
    reset_sampler_state();
    /* The real full-frame callback publishes the stock CSM uniforms once. */
    for (unsigned i = 0; i < 4; ++i)
        record_texture((uint8_t)(9 + i), (bgfx_uniform_handle_t){(uint16_t)(90 + i)},
            (bgfx_texture_handle_t){(uint16_t)(80 + i)}, linear_compare);
}
static void record_texture(uint8_t stage, bgfx_uniform_handle_t sampler,
    bgfx_texture_handle_t texture, uint32_t flags)
{
    TEST_ASSERT_TRUE(stage >= 9 && stage <= 12);
    TEST_ASSERT_TRUE(sampler.idx != UINT16_MAX);
    for (unsigned written = 0; written < pending_sampler_uniform_count; ++written)
        TEST_ASSERT_TRUE_MESSAGE(pending_sampler_uniforms[written] != sampler.idx,
            "sampler uniform set twice before successful submit/discard");
    TEST_ASSERT_LESS_THAN_UINT32(8, pending_sampler_uniform_count);
    pending_sampler_uniforms[pending_sampler_uniform_count++] = sampler.idx;
    unsigned i = stage - 9;
    bound_flags[i] = flags;
    bound_samplers[i] = sampler.idx;
    bound_textures[i] = texture.idx;
}
static void counted_texture(uint8_t stage, bgfx_uniform_handle_t sampler,
    bgfx_texture_handle_t texture, uint32_t flags)
{
    record_texture(stage, sampler, texture, flags);
    ++late_bind_count;
}
static void counted_encoder_texture(bgfx_encoder_t *encoder, uint8_t stage,
    bgfx_uniform_handle_t sampler, bgfx_texture_handle_t texture, uint32_t flags)
{
    (void)encoder;
    ++explicit_bind_count;
    counted_texture(stage, sampler, texture, flags);
}
static void record_submit(bgfx_view_id_t view, bgfx_program_handle_t program,
    uint16_t indirect, uint32_t start, uint32_t num, uint32_t depth, uint8_t flags)
{
    TEST_ASSERT_LESS_THAN_UINT32(8, submit_count);
    ModelSubmit *out = &submits[submit_count++];
    out->view = view; out->program = program.idx; out->indirect = indirect;
    out->start = start; out->num = num; out->depth = depth; out->flags = flags;
    out->instance_start = source_start; out->instance_count = source_count;
    out->callback = callback_count; out->geometry = geometry_count;
    out->late_binds = late_bind_count;
    memcpy(out->sampler_flags, bound_flags, sizeof(bound_flags));
    memcpy(out->samplers, bound_samplers, sizeof(bound_samplers));
    memcpy(out->textures, bound_textures, sizeof(bound_textures));
    out->sampler_uniform_count = pending_sampler_uniform_count;
    memcpy(out->sampler_uniforms, pending_sampler_uniforms,
        pending_sampler_uniform_count * sizeof pending_sampler_uniforms[0]);
    pending_sampler_uniform_count = 0; /* Successful terminal draw boundary only. */
    reset_sampler_state(); /* bgfx consumes this draw's state. */
}
static void counted_submit(bgfx_view_id_t view, bgfx_program_handle_t program,
    uint32_t depth, uint8_t flags)
{ record_submit(view, program, UINT16_MAX, 0, 0, depth, flags); }
static void counted_encoder_submit(bgfx_encoder_t *encoder, bgfx_view_id_t view,
    bgfx_program_handle_t program, uint32_t depth, uint8_t flags)
{
    (void)encoder;
    ++explicit_submit_count;
    counted_submit(view, program, depth, flags);
}
static void counted_indirect(bgfx_view_id_t view, bgfx_program_handle_t program,
    bgfx_indirect_buffer_handle_t indirect, uint32_t start, uint32_t num,
    uint32_t depth, uint8_t flags)
{ record_submit(view, program, indirect.idx, start, num, depth, flags); }
static void counted_discard(uint8_t flags)
{
    TEST_ASSERT_EQUAL_HEX8(BGFX_DISCARD_ALL, flags);
    pending_sampler_uniform_count = 0;
    reset_sampler_state();
}
static void counted_encoder_discard(bgfx_encoder_t *encoder, uint8_t flags)
{ (void)encoder; counted_discard(flags); }
static uint16_t counted_resolve(uint16_t program)
{ ++resolve_count; return program; }
static uint32_t counted_transform(const void *data, uint16_t count)
{ (void)data; return count; }
static void counted_source(bgfx_dynamic_vertex_buffer_handle_t buffer,
                            uint32_t start, uint32_t count)
{
    source_buffer = buffer.idx; source_start = start; source_count = count;
    ++instance_bind_count;
}
static uint32_t provider_available(void *user, uint32_t count, uint16_t stride)
{ (void)user; (void)stride; return count; }
static bool provider_transient(void *user, uint32_t count, uint16_t stride,
                                JceInstanceTransient *out)
{
    (void)user;
    TEST_ASSERT_TRUE(count <= 4 && (stride == 64 || stride == 80));
    memset(out, 0, sizeof(*out));
    out->data = upload_data; out->num = count; out->size = count * stride;
    out->stride = stride; out->handle = 7;
    memset(upload_data, 0xA5, sizeof(upload_data));
    ++transient_count;
    return true;
}
static void provider_bind_transient(void *user, void *encoder,
    const JceInstanceTransient *buffer, uint32_t count)
{
    (void)user;
    TEST_ASSERT_NULL(encoder);
    TEST_ASSERT_TRUE(count <= buffer->num);
    payload_stride = buffer->stride; payload_count = count;
    memcpy(captured_payload, buffer->data, count * buffer->stride);
    source_start = 0; source_count = count;
    ++instance_bind_count;
}
static uint16_t provider_create(void *user, uint32_t count, uint16_t stride)
{ (void)user; (void)count; (void)stride; return UINT16_MAX; }
static void provider_destroy(void *user, uint16_t handle)
{ (void)user; (void)handle; }
static void *provider_staging(void *user, uint32_t size)
{ (void)user; (void)size; return NULL; }
static void provider_free(void *user, void *data)
{ (void)user; (void)data; }
static bool provider_update(void *user, uint16_t handle, uint32_t start,
                            void *data, uint32_t size)
{ (void)user; (void)handle; (void)start; (void)data; (void)size; return false; }
static void provider_bind_dynamic(void *user, void *encoder, uint16_t handle,
                                   uint32_t start, uint32_t count)
{ (void)user; (void)encoder; (void)handle; (void)start; (void)count; }
static const JceInstanceUploadProvider provider = {
    provider_available, provider_transient, provider_create, provider_destroy,
    provider_staging, provider_free, provider_update, provider_bind_dynamic,
    provider_bind_transient, NULL, NULL, NULL, NULL
};
static JcePbrMaterial material_default(void)
{
    JcePbrMaterial out;
    memset(&out, 0, sizeof(out));
    out.custom_program = UINT16_MAX;
    for (unsigned i = 0; i < 4; ++i) out.base_color_factor[i] = 1.0f;
    return out;
}

/* The complete TU also contains unrelated loading, shadow, morph and velocity
 * entries. Their fixed-arity dependency ports are inert; none replaces model
 * admission, primitive iteration, upload packing or terminal submission. */
#define jce_gltf_load(pak, path) ((JceModel *)NULL)
#define jce_gltf_load_memory(data, size, name) ((JceModel *)NULL)
#define jce_gltf_probe_rig_memory(data, size, skin, anim) (false)
#define jce_gltf_decode_cpu(pak, path) ((JceModelCpu *)NULL)
#define jce_gltf_decode_cpu_memory(data, size, name) ((JceModelCpu *)NULL)
#define jce_gltf_upload_cpu(cpu) ((JceModel *)NULL)
#define jce_gltf_model_cpu_free(cpu) ((void)0)
#define jce_mesh_destroy(mesh) ((void)0)
#define jce_skinned_mesh_destroy(mesh) ((void)0)
#define jce_morph_data_destroy(morph) ((void)0)
#define jce_model_texture_pool_destroy(pool) ((void)0)
#define jce_model_texture_pool_gpu_bytes(pool) (UINT64_C(0))
#define jce_texture_destroy(texture) ((void)0)
#define jce_texture_valid(texture) (false)
#define jce_texture_get_size(texture, width, height) ((void)0)
#define jce_skeleton_destroy(skeleton) ((void)0)
#define jce_anim_clip_destroy(clip) ((void)0)
#define jce_morph_weight_track_destroy(track) ((void)0)
#define jce_mesh_vertex_count(mesh) (3u)
#define jce_mesh_index_count(mesh) (3u)
#define jce_skinned_mesh_vertex_count(mesh) (3u)
#define jce_skinned_mesh_index_count(mesh) (3u)
#define jce_mesh_get_vbh(mesh) (7u)
#define jce_mesh_get_ibh(mesh) (8u)
#define jce_skinned_mesh_get_vbh(mesh) (7u)
#define jce_skinned_mesh_get_ibh(mesh) (8u)
#define jce_skinned_mesh_is_skinned(mesh) (false)
#define jce_skinned_mesh_lod_count(mesh) (0u)
#define jce_skinned_mesh_lod_ibh(mesh, level) (8u)
#define jce_skinned_mesh_lod_index_count(mesh, level) (3u)
#define jce_skinned_mesh_meshlet_ibh(mesh) (UINT16_MAX)
#define jce_skinned_mesh_set_submit_state(state) ((void)0)
#define jce_skinned_mesh_set_submit_stencil(stencil) ((void)0)
#define jce_skinned_mesh_set_submit_double_sided(on) ((void)0)
#define jce_skinned_mesh_set_submit_index_range(first, count) ((void)0)
#define jce_skinned_mesh_submit counted_geometry
#define jce_skinned_mesh_cpu_skin_required() (false)
#define jce_skinned_mesh_deform_transient(mesh, palette, count, out) (false)
#define jce_skinned_mesh_submit_vertices(mesh, r, view, source) ((void)0)
#define jce_skinned_mesh_submit_shadow_vertices(mesh, r, view, program, source) ((void)0)
#define jce_skinned_mesh_submit_wireframe_vertices(mesh, r, view, program, source, overlay) ((void)0)
#define jce_skinned_mesh_submit_pick_vertices(mesh, r, view, program, two_sided, source) ((void)0)
#define jce_skinned_mesh_submit_lod counted_geometry_lod
#define jce_skinned_mesh_bind_shadow(mesh) ((void)0)
#define jce_skinned_mesh_set_bones(matrices, count) ((void)0)
#define jce_skinned_mesh_submit_shadow(mesh, r, view, program) ((void)0)
#define jce_skinned_mesh_submit_shadow_lod(mesh, r, view, program, level) ((void)0)
#define jce_skinned_mesh_submit_morphed(mesh, r, view, buffer) ((void)0)
#define jce_skinned_mesh_submit_shadow_morphed(mesh, r, view, program, buffer) ((void)0)
#define jce_skinned_mesh_submit_wireframe_overlay(mesh, r, view) ((void)0)
#define jce_skinned_mesh_submit_pick_id(mesh, r, view, program, double_sided) ((void)0)
#define jce_mesh_submit(mesh, r, view) ((void)0)
#define jce_mesh_submit_shadow(mesh, r, view) ((void)0)
#define jce_mesh_submit_view_wireframe(mesh, r, view, overlay) ((void)0)
#define jce_mesh_submit_pick_id(mesh, r, view, program, double_sided) ((void)0)
#define jce_mesh_submit_pbr_with_program(mesh, r, view, program) ((void)0)
#define jce_skeleton_get_inverse_bind(skeleton, joint) (jce_m4_identity())
#define jce_skeleton_joint_count(skeleton) (0u)
#define jce_skeleton_evaluate(skeleton, locals, out, count) ((void)0)
#define jce_skin_build_world_palette(root, joints, count, out, max) (0u)
#define jce_pbr_material_bind counted_material
#define jce_pbr_material_bind_toon_atlas(atlas, basis) ((void)0)
#define jce_pbr_material_bind_toon_hair_depth(texture) (false)
#define jce_pbr_material_default material_default
#define jce_pbr_material_render_state(mat) (BGFX_STATE_DEFAULT)
#define jce_pbr_material_stencil(mat) (0u)
#define jce_renderer_get_program_pbr(r) ((JceShaderHandle){static_program})
#define jce_renderer_get_program_pbr_skinned(r) ((JceShaderHandle){UINT16_MAX})
#define jce_renderer_get_program_pbr_skinned_toon(r) ((JceShaderHandle){UINT16_MAX})
#define jce_renderer_get_wireframe(r) (false)
#define jce_renderer_get_program_pbr_inst(r) ((JceShaderHandle){instance_program})
#define jce_renderer_get_program_pbr_inst_tint(r) ((JceShaderHandle){tint_program})
#define jce_renderer_get_program_shadow(r) ((JceShaderHandle){UINT16_MAX})
#define jce_renderer_get_program_shadow_skinned(r) ((JceShaderHandle){UINT16_MAX})
#define jce_renderer_get_program_shadow_inst(r) ((JceShaderHandle){UINT16_MAX})
#define jce_renderer_get_program_variant(r, family, key) ((JceShaderHandle){UINT16_MAX})
#define jce_renderer_instance_upload_owner(r) (&upload_owner)
#define jce_renderer_instance_upload_epoch() (3u)
#define bgfx_set_texture counted_texture
#define bgfx_encoder_set_texture counted_encoder_texture
#define bgfx_submit counted_submit
#define bgfx_encoder_submit counted_encoder_submit
#define bgfx_submit_indirect counted_indirect
#define bgfx_set_transform counted_transform
#define bgfx_set_instance_data_from_dynamic_vertex_buffer counted_source
#define bgfx_set_state(state, rgba) ((void)0)
#define bgfx_set_stencil(front, back) ((void)0)
#define bgfx_encoder_set_state(encoder, state, rgba) ((void)0)
#define bgfx_encoder_set_stencil(encoder, front, back) ((void)0)
#define bgfx_set_uniform(handle, value, num) ((void)0)
#define bgfx_set_vertex_buffer(stream, handle, start, count) ((void)0)
#define bgfx_set_index_buffer(handle, start, count) ((void)0)
#define bgfx_discard counted_discard
#define bgfx_encoder_discard counted_encoder_discard
#define jce_shader_deferred_resolve counted_resolve
#include "renderer/jce_render_encoder.h"
#include JCE_TEST_MODEL_COLOUR_OWNER

#if defined(_MSC_VER)
__declspec(thread) bgfx_encoder_t *jce_tls_encoder;
#else
__thread bgfx_encoder_t *jce_tls_encoder;
#endif
uint32_t jce_dbg_xform_matrices;

void setUp(void)
{
    memset(&model, 0, sizeof(model)); memset(&node, 0, sizeof(node));
    memset(primitives, 0, sizeof(primitives)); memset(submits, 0, sizeof(submits));
    submit_count = callback_count = material_count = geometry_count = 0;
    explicit_bind_count = explicit_submit_count = resolve_count = 0;
    transient_count = instance_bind_count = pending_sampler_uniform_count = 0;
    memset(pending_sampler_uniforms, 0, sizeof pending_sampler_uniforms);
    source_buffer = UINT16_MAX; source_start = source_count = 0;
    payload_count = payload_stride = 0;
    memset(captured_payload, 0, sizeof(captured_payload));
    renderer = (const JceRenderer *)&renderer_token;
    material = material_default();
    model.nodes = &node; model.num_nodes = 1;
    model.materials = &material; model.num_materials = 1;
    node.primitives = primitives; node.num_primitives = 2;
    node.local_transform = jce_m4_identity(); node.joint_parent_index = -1;
    for (unsigned i = 0; i < 2; ++i)
        primitives[i].skinned_mesh = (JceSkinnedMesh *)&mesh_tokens[i];
    static_program = 40; instance_program = 41; tint_program = 90;
    /* Model state remains implicit even when an unrelated TLS encoder exists. */
    jce_render_encoder_set((bgfx_encoder_t *)&encoder_token);
    jce_model_set_pre_submit_cb(counted_callback, &model);
    jce_model_set_material_override(NULL); jce_model_set_albedo_checker(false);
    jce_model_set_draw_lod(0); jce_model_set_draw_index_runs(NULL, NULL, 0);
    jce_instance_upload_owner_init(&upload_owner, &provider, NULL, 0, 19);
    reset_sampler_state();
}
void tearDown(void)
{
    jce_model_set_pre_submit_cb(NULL, NULL); jce_model_set_material_override(NULL);
    jce_render_encoder_set(NULL);
    jce_instance_upload_owner_destroy(&upload_owner);
}
static void assert_submits(unsigned count, uint16_t view, uint16_t program)
{
    TEST_ASSERT_EQUAL_UINT32(count, submit_count);
    TEST_ASSERT_EQUAL_UINT32(count, material_count);
    TEST_ASSERT_EQUAL_UINT32(count, geometry_count);
    TEST_ASSERT_EQUAL_UINT32(count, callback_count);
    TEST_ASSERT_EQUAL_UINT32(0, explicit_bind_count);
    TEST_ASSERT_EQUAL_UINT32(0, explicit_submit_count);
    TEST_ASSERT_EQUAL_PTR((bgfx_encoder_t *)&encoder_token, jce_tls_encoder);
    for (unsigned draw = 0; draw < count; ++draw) {
        const ModelSubmit *out = &submits[draw];
        TEST_ASSERT_EQUAL_UINT16(view, out->view);
        TEST_ASSERT_EQUAL_UINT16(program, out->program);
        TEST_ASSERT_EQUAL_UINT32(0, out->depth);
        TEST_ASSERT_EQUAL_UINT8(BGFX_DISCARD_ALL, out->flags);
        TEST_ASSERT_EQUAL_UINT32(draw + 1, out->callback);
        TEST_ASSERT_EQUAL_UINT32(draw + 1, out->geometry);
        TEST_ASSERT_EQUAL_UINT32(0, out->late_binds);
        TEST_ASSERT_EQUAL_UINT32(4, out->sampler_uniform_count);
        for (unsigned ci = 0; ci < 4; ++ci) {
            TEST_ASSERT_EQUAL_UINT32(linear_compare, out->sampler_flags[ci]);
            TEST_ASSERT_EQUAL_UINT16(90 + ci, out->samplers[ci]);
            TEST_ASSERT_EQUAL_UINT16(90 + ci, out->sampler_uniforms[ci]);
            TEST_ASSERT_EQUAL_UINT16(80 + ci, out->textures[ci]);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(count, resolve_count);
    TEST_ASSERT_EQUAL_UINT32(0, pending_sampler_uniform_count);
}
static void roots_init(jce_mat4 roots[2])
{
    roots[0] = jce_m4_identity(); roots[1] = jce_m4_identity();
    roots[0].raw[3][0] = 2.5f; roots[1].raw[3][2] = -7.25f;
}
static void solo_submits_after_each_primitive_callback(void)
{
    jce_model_draw(&model, renderer, 23, NULL, NULL, 0);
    assert_submits(2, 23, 40);
    TEST_ASSERT_EQUAL_UINT32(0, instance_bind_count);
}
static void cpu_instances_keep_matrix_payload(void)
{
    jce_mat4 roots[2]; roots_init(roots);
    jce_model_draw_instanced(&model, renderer, 23, roots, 2);
    assert_submits(2, 23, 41);
    TEST_ASSERT_EQUAL_UINT32(2, transient_count);
    TEST_ASSERT_EQUAL_UINT32(2, instance_bind_count);
    TEST_ASSERT_EQUAL_UINT16(64, payload_stride); TEST_ASSERT_EQUAL_UINT32(2, payload_count);
    TEST_ASSERT_EQUAL_MEMORY(roots, captured_payload, sizeof(roots));
}
static void fixed_buffer_keeps_instance_slice(void)
{
    jce_model_draw_instanced_from_buffer(&model, renderer, 23, 117, 29, 53, 1);
    assert_submits(1, 23, 41);
    TEST_ASSERT_EQUAL_UINT16(117, source_buffer);
    TEST_ASSERT_EQUAL_UINT32(29, submits[0].instance_start);
    TEST_ASSERT_EQUAL_UINT32(53, submits[0].instance_count);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, submits[0].indirect);
    TEST_ASSERT_EQUAL_UINT32(1, instance_bind_count);
}
static void indirect_keeps_argument_element_and_instance_source(void)
{
    jce_model_draw_indirect_from_buffer(&model, renderer, 23, 118, 119, 37, 1);
    assert_submits(1, 23, 41);
    TEST_ASSERT_EQUAL_UINT16(118, source_buffer);
    TEST_ASSERT_EQUAL_UINT16(119, submits[0].indirect);
    TEST_ASSERT_EQUAL_UINT32(37, submits[0].start); TEST_ASSERT_EQUAL_UINT32(1, submits[0].num);
    TEST_ASSERT_EQUAL_UINT32(0, submits[0].instance_start);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, submits[0].instance_count);
    TEST_ASSERT_EQUAL_UINT32(1, instance_bind_count);
}
static void custom_material_program_is_preserved(void)
{
    material.custom_program = 71;
    jce_model_draw(&model, renderer, 23, NULL, NULL, 0);
    assert_submits(2, 23, 71);
}
static void alternate_view_preserves_stock_program(void)
{
    jce_model_draw(&model, renderer, 24, NULL, NULL, 0);
    assert_submits(2, 24, 40);
}
static void genuine_tinted_instances_keep_80_byte_payload(void)
{
    jce_mat4 roots[2]; roots_init(roots);
    const jce_vec4 tints[2] = {{0.2f, 0.4f, 0.6f, 0.8f}, {0.1f, 0.3f, 0.5f, 0.7f}};
    jce_model_draw_instanced_tinted(&model, renderer, 23, roots, tints, 2);
    assert_submits(2, 23, 90);
    TEST_ASSERT_EQUAL_UINT16(80, payload_stride); TEST_ASSERT_EQUAL_UINT32(2, payload_count);
    for (unsigned i = 0; i < 2; ++i) {
        TEST_ASSERT_EQUAL_MEMORY(&roots[i], captured_payload + i * 80, 64);
        TEST_ASSERT_EQUAL_MEMORY(&tints[i], captured_payload + i * 80 + 64, 16);
    }
}
static void single_tint_folds_material_into_stock_static_program(void)
{
    jce_mat4 root = jce_m4_identity();
    const jce_vec4 tint = {0.25f, 0.5f, 0.75f, 0.9f};
    jce_model_draw_instanced_tinted(&model, renderer, 23, &root, &tint, 1);
    assert_submits(2, 23, 40);
    TEST_ASSERT_EQUAL_FLOAT_ARRAY(&tint.x, last_material.base_color_factor, 4);
    TEST_ASSERT_NULL(jce_model_get_material_override());
    TEST_ASSERT_EQUAL_UINT32(0, transient_count);
}
static void assert_foliage_bands(unsigned bands)
{
    TEST_ASSERT_EQUAL_UINT32(bands, submit_count);
    TEST_ASSERT_EQUAL_UINT32(bands, material_count);
    TEST_ASSERT_EQUAL_UINT32(bands, callback_count);
    TEST_ASSERT_EQUAL_UINT32(bands, instance_bind_count);
    for (unsigned band = 0; band < bands; ++band) {
        TEST_ASSERT_EQUAL_UINT16(41u, submits[band].program);
        TEST_ASSERT_EQUAL_UINT16(119u, submits[band].indirect);
        TEST_ASSERT_EQUAL_UINT32(band, submits[band].start);
        TEST_ASSERT_EQUAL_UINT32(1u, submits[band].num);
        TEST_ASSERT_EQUAL_UINT32(band + 1u, submits[band].callback);
        TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, submits[band].instance_count);
        TEST_ASSERT_EQUAL_UINT8(BGFX_DISCARD_ALL, submits[band].flags);
        TEST_ASSERT_EQUAL_UINT32(4, submits[band].sampler_uniform_count);
        for (unsigned ci = 0; ci < 4; ++ci) {
            TEST_ASSERT_EQUAL_UINT16(90 + ci, submits[band].samplers[ci]);
            TEST_ASSERT_EQUAL_UINT16(90 + ci, submits[band].sampler_uniforms[ci]);
        }
    }
}
static void foliage_lod_missing_material_uses_default_in_each_band(void)
{
    node.num_primitives = 1u; model.num_materials = 0u; model.materials = NULL;
    jce_model_draw_foliage_lod_indirect(&model, renderer, 23u, 41u, 3u, 118u, 119u);
    assert_foliage_bands(3u);
    const JcePbrMaterial expected = material_default();
    TEST_ASSERT_EQUAL_MEMORY(&expected, &last_material, sizeof expected);
}
static void foliage_lod_material_override_precedes_default_in_each_band(void)
{
    node.num_primitives = 1u; model.num_materials = 0u; model.materials = NULL;
    JcePbrMaterial override = material_default();
    override.base_color_factor[0] = .25f; override.roughness_factor = .66f;
    override.double_sided = true;
    jce_model_set_material_override(&override);
    jce_model_draw_foliage_lod_indirect(&model, renderer, 23u, 41u, 2u, 118u, 119u);
    assert_foliage_bands(2u);
    TEST_ASSERT_EQUAL_MEMORY(&override, &last_material, sizeof override);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(solo_submits_after_each_primitive_callback);
    RUN_TEST(cpu_instances_keep_matrix_payload);
    RUN_TEST(fixed_buffer_keeps_instance_slice);
    RUN_TEST(indirect_keeps_argument_element_and_instance_source);
    RUN_TEST(custom_material_program_is_preserved);
    RUN_TEST(alternate_view_preserves_stock_program);
    RUN_TEST(genuine_tinted_instances_keep_80_byte_payload);
    RUN_TEST(single_tint_folds_material_into_stock_static_program);
    RUN_TEST(foliage_lod_missing_material_uses_default_in_each_band);
    RUN_TEST(foliage_lod_material_override_precedes_default_in_each_band);
    return UNITY_END();
}
